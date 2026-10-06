#include "knob.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// EC1_A/EC1_B -- see spec.md's pin reference table.
#define ENC_A_GPIO 8
#define ENC_B_GPIO 7

#define POLL_MS 20
#define COUNTS_PER_STEP 1
#define BUTTON_DEBOUNCE_POLLS 3

static const char *TAG = "knob";

static pcnt_unit_handle_t s_pcnt_unit;
static knob_rotate_cb_t s_on_rotate;
static knob_press_cb_t s_on_press;

static void pcnt_init(void) {
    pcnt_unit_config_t unit_cfg = {.low_limit = -1000, .high_limit = 1000};
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &s_pcnt_unit));

    pcnt_glitch_filter_config_t filter_cfg = {.max_glitch_ns = 1000};
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt_unit, &filter_cfg));

    // This knob isn't full quadrature: one detent pulses only A (one
    // direction) or only B (the other), never both, so a decoder that
    // needs the A/B sequence never completes a step. Count each line's
    // falling edges instead.
    pcnt_chan_config_t chan_a_cfg = {.edge_gpio_num = ENC_A_GPIO, .level_gpio_num = -1};
    pcnt_channel_handle_t chan_a;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt_unit, &chan_a_cfg, &chan_a));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_a, PCNT_CHANNEL_EDGE_ACTION_HOLD, PCNT_CHANNEL_EDGE_ACTION_INCREASE));

    pcnt_chan_config_t chan_b_cfg = {.edge_gpio_num = ENC_B_GPIO, .level_gpio_num = -1};
    pcnt_channel_handle_t chan_b;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt_unit, &chan_b_cfg, &chan_b));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_b, PCNT_CHANNEL_EDGE_ACTION_HOLD, PCNT_CHANNEL_EDGE_ACTION_DECREASE));

    // PCNT only routes the pins as inputs; without pull-ups a mechanical
    // encoder's open contacts float and no edges are counted.
    ESP_ERROR_CHECK(gpio_set_pull_mode(ENC_A_GPIO, GPIO_PULLUP_ONLY));
    ESP_ERROR_CHECK(gpio_set_pull_mode(ENC_B_GPIO, GPIO_PULLUP_ONLY));

    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt_unit));
}

static void knob_task(void *arg) {
    (void)arg;
    int accum = 0;
    int button_low_polls = 0;
    bool button_down = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        int count = 0;
        pcnt_unit_get_count(s_pcnt_unit, &count);
        pcnt_unit_clear_count(s_pcnt_unit);
        accum += count;

        int steps = accum / COUNTS_PER_STEP;
        if (steps != 0) {
            accum -= steps * COUNTS_PER_STEP;
            s_on_rotate(steps);
        }

        bool pressed_now = gpio_get_level(ENC_A_GPIO) == 0 && gpio_get_level(ENC_B_GPIO) == 0;
        if (!pressed_now) {
            button_low_polls = 0;
        } else if (button_low_polls < BUTTON_DEBOUNCE_POLLS) {
            button_low_polls++;
        }

        bool debounced_down = button_low_polls >= BUTTON_DEBOUNCE_POLLS;
        if (debounced_down && !button_down) {
            ESP_LOGI(TAG, "SW2 pressed");
            s_on_press();
        }
        button_down = debounced_down;
    }
}

void knob_start(knob_rotate_cb_t on_rotate, knob_press_cb_t on_press) {
    s_on_rotate = on_rotate;
    s_on_press = on_press;
    pcnt_init();
    xTaskCreate(knob_task, "knob", 3072, NULL, 5, NULL);
}
