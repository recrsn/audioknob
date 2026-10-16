#ifndef AK_NATIVE

#include "audio_source.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "uart_link.h"

#define PIN_I2S_SWITCH GPIO_NUM_0

// CH445P: IN=0 selects the S1 bank, IN=1 the S2 bank. The DAC schematic
// sheet wires ESP32_I2S_* to S1 and S3_I2S_* to S2 (Waveshare's own S3
// audio demo relies on GPIO0's boot pull-up to reach the DAC).
#define SWITCH_LEVEL_ESP32 0
#define SWITCH_LEVEL_S3 1

#define WATCH_PERIOD_US (50 * 1000)

static const char *TAG = "audio_source";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_source_t s_active = AUDIO_SOURCE_ESP32;
static bool s_esp32_was_playing;
static void (*s_on_airplay_preempted)(void);

// Caller holds s_lock, so the GPIO level can never disagree with s_active.
static void select_locked(audio_source_t source) {
    s_active = source;
    gpio_set_level(PIN_I2S_SWITCH, source == AUDIO_SOURCE_AIRPLAY ? SWITCH_LEVEL_S3 : SWITCH_LEVEL_ESP32);
}

// Only a rising edge of A2DP playback counts as "started": when AirPlay
// takes the DAC, A2DP keeps reporting playing until our pause lands, and
// must not grab the DAC straight back in that window.
static void esp32_watch_cb(void *arg) {
    (void)arg;
    wireless_state_t state;
    bool playing = uart_link_get_latest(&state) && state.connected && state.playing;
    bool started = playing && !s_esp32_was_playing;
    s_esp32_was_playing = playing;
    if (!started) {
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    bool preempted = s_active == AUDIO_SOURCE_AIRPLAY;
    select_locked(AUDIO_SOURCE_ESP32);
    taskEXIT_CRITICAL(&s_lock);

    if (preempted) {
        ESP_LOGI(TAG, "A2DP started: taking the DAC back from AirPlay");
        s_on_airplay_preempted();
    }
}

void audio_source_init(void (*on_airplay_preempted)(void)) {
    s_on_airplay_preempted = on_airplay_preempted;

    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_I2S_SWITCH,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    taskENTER_CRITICAL(&s_lock);
    select_locked(AUDIO_SOURCE_ESP32);
    taskEXIT_CRITICAL(&s_lock);

    const esp_timer_create_args_t timer_args = {
        .callback = esp32_watch_cb,
        .name = "src_watch",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, WATCH_PERIOD_US));
}

void audio_source_claim_airplay(void) {
    taskENTER_CRITICAL(&s_lock);
    select_locked(AUDIO_SOURCE_AIRPLAY);
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "AirPlay has the DAC");

    wireless_state_t esp32;
    if (uart_link_get_latest(&esp32) && esp32.connected && esp32.playing) {
        ESP_LOGI(TAG, "pausing A2DP");
        uart_link_send_command(CMD_PAUSE);
    }
}

void audio_source_release_airplay(void) {
    taskENTER_CRITICAL(&s_lock);
    if (s_active == AUDIO_SOURCE_AIRPLAY) {
        select_locked(AUDIO_SOURCE_ESP32);
    }
    taskEXIT_CRITICAL(&s_lock);
}

audio_source_t audio_source_active(void) {
    taskENTER_CRITICAL(&s_lock);
    audio_source_t active = s_active;
    taskEXIT_CRITICAL(&s_lock);
    return active;
}

#endif  // !AK_NATIVE
