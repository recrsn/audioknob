#include "audio_source.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "uart_link.h"

#define PIN_I2S_SWITCH GPIO_NUM_0

// CH445P: IN=0 selects the S1 bank, IN=1 the S2 bank. The DAC schematic
// sheet wires ESP32_I2S_* to S1 and S3_I2S_* to S2 (Waveshare's own S3
// audio demo relies on GPIO0's boot pull-up to reach the DAC).
#define SWITCH_LEVEL_ESP32 0
#define SWITCH_LEVEL_S3 1

#define WATCH_PERIOD_MS 50
#define WATCH_TASK_STACK 3072

static const char *TAG = "audio_source";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static audio_source_t s_active = AUDIO_SOURCE_ESP32;
static bool s_esp32_was_playing;
static bool s_suspended;

static void pause_a2dp(void);
static void (*s_on_airplay_preempted)(void);
static void (*s_on_local_preempted)(void);

// Caller holds s_lock, so the GPIO level can never disagree with s_active.
static void select_locked(audio_source_t source) {
    s_active = source;
    gpio_set_level(PIN_I2S_SWITCH, source == AUDIO_SOURCE_ESP32 ? SWITCH_LEVEL_ESP32 : SWITCH_LEVEL_S3);
}

// Only a rising edge of A2DP playback counts as "started": when AirPlay
// takes the DAC, A2DP keeps reporting playing until our pause lands, and
// must not grab the DAC straight back in that window.
static void esp32_watch(void) {
    wireless_state_t state;
    bool playing = uart_link_get_latest(&state) && state.connected && state.playing;
    bool started = playing && !s_esp32_was_playing;
    s_esp32_was_playing = playing;
    if (!started) {
        return;
    }

    taskENTER_CRITICAL(&s_lock);
    if (s_suspended) {
        taskEXIT_CRITICAL(&s_lock);
        pause_a2dp();
        return;
    }
    audio_source_t was = s_active;
    select_locked(AUDIO_SOURCE_ESP32);
    taskEXIT_CRITICAL(&s_lock);

    if (was == AUDIO_SOURCE_AIRPLAY) {
        ESP_LOGI(TAG, "A2DP started: taking the DAC back from AirPlay");
        s_on_airplay_preempted();
    } else if (was == AUDIO_SOURCE_LOCAL && s_on_local_preempted) {
        ESP_LOGI(TAG, "A2DP started: taking the DAC from local audio");
        s_on_local_preempted();
    }
}

// A task rather than an esp_timer: the check blocks on uart_link's mutex
// and the preempt flushes the player, neither of which belongs on the
// shared timer task.
static void esp32_watch_task(void *arg) {
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(WATCH_PERIOD_MS));
        esp32_watch();
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

    BaseType_t ok = xTaskCreate(esp32_watch_task, "src_watch", WATCH_TASK_STACK, NULL, 5, NULL);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

static void pause_a2dp(void) {
    wireless_state_t esp32;
    if (uart_link_get_latest(&esp32) && esp32.connected && esp32.playing) {
        ESP_LOGI(TAG, "pausing A2DP");
        uart_link_send_command(CMD_PAUSE);
    }
}

bool audio_source_claim_airplay(void) {
    taskENTER_CRITICAL(&s_lock);
    if (s_suspended) {
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    bool local = s_active == AUDIO_SOURCE_LOCAL;
    select_locked(AUDIO_SOURCE_AIRPLAY);
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "AirPlay has the DAC");

    if (local && s_on_local_preempted) {
        s_on_local_preempted();
    }
    pause_a2dp();
    return true;
}

void audio_source_release_airplay(void) {
    taskENTER_CRITICAL(&s_lock);
    if (!s_suspended && s_active == AUDIO_SOURCE_AIRPLAY) {
        select_locked(AUDIO_SOURCE_ESP32);
    }
    taskEXIT_CRITICAL(&s_lock);
}

void audio_source_set_local_preempted(void (*on_preempted)(void)) {
    s_on_local_preempted = on_preempted;
}

bool audio_source_claim_local(void) {
    taskENTER_CRITICAL(&s_lock);
    if (s_suspended) {
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    bool airplay = s_active == AUDIO_SOURCE_AIRPLAY;
    select_locked(AUDIO_SOURCE_LOCAL);
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "local audio has the DAC");

    if (airplay) {
        s_on_airplay_preempted();
    }
    pause_a2dp();
    return true;
}

void audio_source_release_local(void) {
    taskENTER_CRITICAL(&s_lock);
    if (!s_suspended && s_active == AUDIO_SOURCE_LOCAL) {
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

void audio_source_suspend(bool suspended) {
    taskENTER_CRITICAL(&s_lock);
    s_suspended = suspended;
    // Keep Bluetooth off the DAC even while its remote pause is in flight.
    select_locked(suspended ? AUDIO_SOURCE_AIRPLAY : AUDIO_SOURCE_ESP32);
    taskEXIT_CRITICAL(&s_lock);
    if (suspended) {
        s_on_airplay_preempted();
        if (s_on_local_preempted) {
            s_on_local_preempted();
        }
        pause_a2dp();
    }
}

bool audio_source_suspended(void) {
    taskENTER_CRITICAL(&s_lock);
    bool suspended = s_suspended;
    taskEXIT_CRITICAL(&s_lock);
    return suspended;
}
