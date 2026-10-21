#include "drivers/knob.h"

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"

#define ENC_A_GPIO 8
#define ENC_B_GPIO 7
#define EDGE_QUEUE_SIZE 64
#define MIN_LOW_MS 6
#define REVERSE_LOCKOUT_MS 60
#define BUTTON_DEBOUNCE_MS 30
#define LONG_PRESS_MS 500
#define PRESS_SETTLE_MS 100
#define BOTH_HIGH 3

// This is a direction-pulse encoder, not quadrature. SW2 grounds both
// direction lines. Keep raw edges so LVGL rendering cannot hide a pulse.
typedef struct {
    uint32_t ms;
    uint8_t levels;
} edge_t;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static edge_t s_edges[EDGE_QUEUE_SIZE];
static unsigned s_head, s_tail;
static bool s_overflow;
static edge_t s_levels;
static uint32_t s_low_since[2], s_button_since, s_down_since;
static uint32_t s_last_both_low, s_last_step;
static int s_last_dir;
static bool s_down, s_long_sent, s_resync;
static knob_rotate_cb_t s_on_rotate;
static knob_press_cb_t s_on_press, s_on_long_press;

static edge_t pins(void) {
    return (edge_t){.ms = (uint32_t)(esp_timer_get_time() / 1000),
                    .levels = gpio_get_level(ENC_A_GPIO) | (gpio_get_level(ENC_B_GPIO) << 1)};
}

static void edge_isr(void *arg) {
    (void)arg;
    portENTER_CRITICAL_ISR(&s_mux);
    edge_t edge = pins();
    unsigned next = (s_head + 1) % EDGE_QUEUE_SIZE;
    if (next == s_tail) {
        s_overflow = true;
    } else {
        s_edges[s_head] = edge;
        s_head = next;
    }
    portEXIT_CRITICAL_ISR(&s_mux);
}

static void reset_decoder(edge_t edge) {
    s_levels = edge;
    s_low_since[0] = s_low_since[1] = edge.ms;
    s_button_since = s_last_both_low = edge.ms;
    s_last_step = edge.ms;
    s_last_dir = 0;
    s_down = s_long_sent = false;
}

// Produce timed button transitions before the next edge. Replaying their
// timestamps preserves short/long classification even after a slow frame.
static bool button_until(uint32_t until, lv_indev_data_t *data) {
    if (s_resync) return false;
    bool pressed = s_levels.levels == 0;
    if (pressed != s_down && until - s_button_since >= BUTTON_DEBOUNCE_MS) {
        s_down = pressed;
        s_down_since = s_button_since + BUTTON_DEBOUNCE_MS;
        s_long_sent = false;
        data->timestamp = s_down_since;
        data->state = s_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        return true;
    }
    if (s_down && !s_long_sent && until - s_down_since >= LONG_PRESS_MS) {
        s_long_sent = true;
        data->timestamp = s_down_since + LONG_PRESS_MS;
        data->state = LV_INDEV_STATE_PRESSED;
        return true;
    }
    return false;
}

static int decode_edge(edge_t edge) {
    uint8_t old = s_levels.levels;
    bool was_pressed = old == 0;
    if (was_pressed) s_last_both_low = edge.ms;
    int step = 0;
    for (unsigned i = 0; i < 2; ++i) {
        unsigned mask = 1U << i;
        if (!(edge.levels & mask) && (old & mask)) s_low_since[i] = edge.ms;
        if ((edge.levels & mask) && !(old & mask) && edge.ms - s_low_since[i] >= MIN_LOW_MS &&
            edge.ms - s_last_both_low >= PRESS_SETTLE_MS) {
            int dir = i == 0 ? 1 : -1;
            if (dir == s_last_dir || edge.ms - s_last_step >= REVERSE_LOCKOUT_MS) {
                step = dir;
                s_last_dir = dir;
                s_last_step = edge.ms;
            }
        }
    }
    if (was_pressed != (edge.levels == 0)) s_button_since = edge.ms;
    s_levels = edge;
    if (s_resync) {
        if (edge.levels == BOTH_HIGH) s_resync = false;
        return 0;
    }
    return step;
}

static void encoder_read(lv_indev_t *indev, lv_indev_data_t *data) {
    data->key = LV_KEY_ENTER;
    for (;;) {
        portENTER_CRITICAL(&s_mux);
        bool overflow = s_overflow;
        if (overflow) {
            s_tail = s_head;
            s_overflow = false;
        }
        bool pending = s_tail != s_head;
        edge_t edge = pending ? s_edges[s_tail] : pins();
        portEXIT_CRITICAL(&s_mux);
        if (overflow) {
            // Lost transitions cannot safely become clicks. Reset the LVGL
            // press too, then resume from the actual pins without a turn.
            lv_indev_reset(indev, NULL);
            lv_indev_wait_release(indev);
            reset_decoder(edge);
            s_resync = edge.levels != BOTH_HIGH;
        }
        if (button_until(edge.ms, data)) {
            data->continue_reading = true;
            return;
        }
        if (!pending) {
            data->state = s_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
            data->timestamp = edge.ms;
            return;
        }
        portENTER_CRITICAL(&s_mux);
        s_tail = (s_tail + 1) % EDGE_QUEUE_SIZE;
        portEXIT_CRITICAL(&s_mux);
        int step = decode_edge(edge);
        if (step) {
            data->enc_diff = step;
            data->state = s_down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
            data->timestamp = edge.ms;
            data->continue_reading = true;
            return;
        }
    }
}

static void encoder_event(lv_event_t *event) {
    switch (lv_event_get_code(event)) {
    case LV_EVENT_KEY: {
        uint32_t key = lv_event_get_key(event);
        if (key == LV_KEY_LEFT) s_on_rotate(-1);
        if (key == LV_KEY_RIGHT) s_on_rotate(1);
        break;
    }
    case LV_EVENT_SHORT_CLICKED: s_on_press(); break;
    case LV_EVENT_LONG_PRESSED: s_on_long_press(); break;
    default: break;
    }
}

void knob_start(knob_rotate_cb_t on_rotate, knob_press_cb_t on_press, knob_press_cb_t on_long_press) {
    s_on_rotate = on_rotate;
    s_on_press = on_press;
    s_on_long_press = on_long_press;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << ENC_A_GPIO) | (1ULL << ENC_B_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    reset_decoder(pins());

    lv_group_t *group = lv_group_create();
    lv_obj_t *sink = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(sink);
    lv_obj_set_size(sink, 0, 0);
    lv_obj_set_scrollable(sink, false);
    lv_obj_set_click_focusable(sink, false);
    lv_obj_add_event_cb(sink, encoder_event, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(sink, encoder_event, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(sink, encoder_event, LV_EVENT_LONG_PRESSED, NULL);
    lv_group_add_obj(group, sink);
    lv_group_set_editing(group, true);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev, encoder_read);
    lv_indev_set_long_press_time(indev, LONG_PRESS_MS);
    lv_indev_set_group(indev, group);

    esp_err_t err = gpio_install_isr_service(0);
    ESP_ERROR_CHECK(err == ESP_ERR_INVALID_STATE ? ESP_OK : err);
    ESP_ERROR_CHECK(gpio_isr_handler_add(ENC_A_GPIO, edge_isr, NULL));
    ESP_ERROR_CHECK(gpio_isr_handler_add(ENC_B_GPIO, edge_isr, NULL));
}
