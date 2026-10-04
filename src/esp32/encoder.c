#include "encoder.h"

#include <stdbool.h>

#include "app_state.h"
#include "avrcp_controller.h"
#include "avrcp_target.h"
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// EC2_A/EC2_B — see repo spec's pin reference table. SW1 is wired as a
// short-to-GND across both pins (not a separate GPIO), so a press reads as
// both lines low at once.
#define ENC_A_GPIO 19
#define ENC_B_GPIO 22

#define ENC_POLL_MS 20
#define ENC_COUNTS_PER_STEP 4  // typical mechanical quadrature encoder detent
#define BUTTON_DEBOUNCE_POLLS 3

static const char *TAG = "encoder";

static pcnt_unit_handle_t s_pcnt_unit;

static void pcnt_init(void) {
    pcnt_unit_config_t unit_cfg = {.low_limit = -1000, .high_limit = 1000};
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &s_pcnt_unit));

    pcnt_glitch_filter_config_t filter_cfg = {.max_glitch_ns = 1000};
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt_unit, &filter_cfg));

    pcnt_chan_config_t chan_a_cfg = {.edge_gpio_num = ENC_A_GPIO, .level_gpio_num = ENC_B_GPIO};
    pcnt_channel_handle_t chan_a;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt_unit, &chan_a_cfg, &chan_a));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    pcnt_chan_config_t chan_b_cfg = {.edge_gpio_num = ENC_B_GPIO, .level_gpio_num = ENC_A_GPIO};
    pcnt_channel_handle_t chan_b;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt_unit, &chan_b_cfg, &chan_b));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    // PCNT only routes the pins as inputs; without pull-ups a mechanical
    // encoder's open contacts float and no edges are counted.
    ESP_ERROR_CHECK(gpio_set_pull_mode(ENC_A_GPIO, GPIO_PULLUP_ONLY));
    ESP_ERROR_CHECK(gpio_set_pull_mode(ENC_B_GPIO, GPIO_PULLUP_ONLY));

    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt_unit));
}

static void encoder_task(void *arg) {
    (void)arg;
    int accum = 0;
    int button_low_polls = 0;
    bool button_down = false;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(ENC_POLL_MS));

        int count = 0;
        pcnt_unit_get_count(s_pcnt_unit, &count);
        pcnt_unit_clear_count(s_pcnt_unit);
        accum += count;

        int steps = accum / ENC_COUNTS_PER_STEP;
        if (steps != 0) {
            accum -= steps * ENC_COUNTS_PER_STEP;
            int volume = (int)app_state_get().volume + steps;
            if (volume < 0) {
                volume = 0;
            } else if (volume > 100) {
                volume = 100;
            }
            avrcp_target_set_local_volume((uint8_t)volume);
        }

        bool pressed_now = gpio_get_level(ENC_A_GPIO) == 0 && gpio_get_level(ENC_B_GPIO) == 0;
        if (pressed_now) {
            if (button_low_polls < BUTTON_DEBOUNCE_POLLS) {
                button_low_polls++;
            }
        } else {
            button_low_polls = 0;
        }

        bool debounced_down = button_low_polls >= BUTTON_DEBOUNCE_POLLS;
        if (debounced_down && !button_down) {
            ESP_LOGI(TAG, "SW1 pressed");
            avrcp_controller_toggle_play_pause();
        }
        button_down = debounced_down;
    }
}

void encoder_start(void) {
    pcnt_init();
    xTaskCreate(encoder_task, "encoder", 3072, NULL, 5, NULL);
}
