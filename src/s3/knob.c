#include "knob.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// EC1_A/EC1_B -- see spec.md's pin reference table.
#define ENC_A_GPIO 8
#define ENC_B_GPIO 7

#define POLL_MS 3
#define MIN_LOW_POLLS 2
// Both lines pulse a little on every detent; the weaker one is a bounce,
// not a reversal, so a human can't be reversing this soon.
#define REVERSE_LOCKOUT_MS 60
#define BUTTON_DEBOUNCE_POLLS 10
#define LONG_PRESS_MS 500
// The button shorts both lines, so releasing it looks like a pulse on each.
#define PRESS_SETTLE_MS 100

static const char *TAG = "knob";

static knob_rotate_cb_t s_on_rotate;
static knob_press_cb_t s_on_press;
static knob_press_cb_t s_on_long_press;

// This knob isn't quadrature: a detent pulses only A (one direction) or
// only B (the other), as in Waveshare's own demo (bidi_switch_knob.c). A
// pulse counts on its rising edge, once the line has been low long enough
// to not be contact bounce.
typedef struct {
    int gpio;
    int dir;
    int level;
    int low_polls;
} channel_t;

static bool channel_pulsed(channel_t *ch) {
    int level = gpio_get_level(ch->gpio);
    bool pulsed = level == 1 && ch->level == 0 && ch->low_polls >= MIN_LOW_POLLS;
    ch->low_polls = level == 0 ? ch->low_polls + 1 : 0;
    ch->level = level;
    return pulsed;
}

typedef struct {
    int low_polls;
    int high_polls;
    bool down;
    bool long_sent;
    TickType_t down_tick;
} button_t;

// A short press only counts once released, since until then it may still
// become a long one; a long press fires as soon as it qualifies, so the user
// knows to let go.
static void button_poll(button_t *btn, bool pressed_now, TickType_t now) {
    btn->low_polls = pressed_now ? btn->low_polls + 1 : 0;
    btn->high_polls = pressed_now ? 0 : btn->high_polls + 1;
    if (!btn->down) {
        if (btn->low_polls >= BUTTON_DEBOUNCE_POLLS) {
            btn->down = true;
            btn->long_sent = false;
            btn->down_tick = now;
        }
        return;
    }
    if (btn->high_polls >= BUTTON_DEBOUNCE_POLLS) {
        btn->down = false;
        if (!btn->long_sent) {
            ESP_LOGI(TAG, "SW2 pressed");
            s_on_press();
        }
    } else if (!btn->long_sent && now - btn->down_tick >= pdMS_TO_TICKS(LONG_PRESS_MS)) {
        btn->long_sent = true;
        ESP_LOGI(TAG, "SW2 long-pressed");
        s_on_long_press();
    }
}

static void gpio_init(int gpio) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

static void knob_task(void *arg) {
    (void)arg;
    channel_t a = {.gpio = ENC_A_GPIO, .dir = 1, .level = 1};
    channel_t b = {.gpio = ENC_B_GPIO, .dir = -1, .level = 1};
    channel_t *channels[] = {&a, &b};
    int last_dir = 0;
    TickType_t last_step = 0;
    button_t button = {0};
    TickType_t last_both_low = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        TickType_t now = xTaskGetTickCount();

        for (int i = 0; i < 2; i++) {
            bool pulsed = channel_pulsed(channels[i]);
            bool near_press = now - last_both_low < pdMS_TO_TICKS(PRESS_SETTLE_MS);
            if (pulsed) ESP_LOGI(TAG, "DBG t=%u ch=%d near_press=%d", (unsigned)now, i, near_press);
            if (!pulsed || near_press) {
                continue;
            }
            int dir = channels[i]->dir;
            bool reversal_too_soon =
                dir != last_dir && now - last_step < pdMS_TO_TICKS(REVERSE_LOCKOUT_MS);
            if (reversal_too_soon) {
                ESP_LOGI(TAG, "DBG t=%u reversal dropped", (unsigned)now);
                continue;
            }
            last_dir = dir;
            last_step = now;
            s_on_rotate(dir);
        }

        bool pressed_now = a.level == 0 && b.level == 0;
        if (pressed_now) {
            last_both_low = now;
        }
        button_poll(&button, pressed_now, now);
    }
}

void knob_start(knob_rotate_cb_t on_rotate, knob_press_cb_t on_press, knob_press_cb_t on_long_press) {
    s_on_rotate = on_rotate;
    s_on_press = on_press;
    s_on_long_press = on_long_press;
    gpio_init(ENC_A_GPIO);
    gpio_init(ENC_B_GPIO);
    xTaskCreate(knob_task, "knob", 3072, NULL, 5, NULL);
}
