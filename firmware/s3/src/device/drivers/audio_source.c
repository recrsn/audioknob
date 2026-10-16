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
static bool s_s3_held;
static bool s_esp32_was_playing;
static bool s_suspended;
static void pause_a2dp(void);
static void (*s_on_preempted)(audio_source_t source);
static void (*s_on_local_preempted)(void);

// Caller holds s_lock, so the GPIO level can never disagree with s_active.
static void select_locked(audio_source_t source) {
    s_active = source;
    bool s3 = source != AUDIO_SOURCE_ESP32 || s_s3_held;
    gpio_set_level(PIN_I2S_SWITCH, s3 ? SWITCH_LEVEL_S3 : SWITCH_LEVEL_ESP32);
}

// Takes the DAC for `source` and tells whichever of this MCU's sources had
// it to stop.
static bool take(audio_source_t source) {
    taskENTER_CRITICAL(&s_lock);
    if (s_suspended) {
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    audio_source_t prev = s_active;
    select_locked(source);
    taskEXIT_CRITICAL(&s_lock);
    if (prev != source && prev != AUDIO_SOURCE_ESP32) {
        if (prev == AUDIO_SOURCE_LOCAL && s_on_local_preempted) {
            s_on_local_preempted();
        } else {
            s_on_preempted(prev);
        }
    }
    return true;
}

// Only a rising edge of A2DP playback counts as "started": when this MCU
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

    if (audio_source_active() != AUDIO_SOURCE_ESP32) {
        ESP_LOGI(TAG, "A2DP started: taking the DAC back");
    }
    if (!take(AUDIO_SOURCE_ESP32)) {
        pause_a2dp();
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

void audio_source_init(void (*on_preempted)(audio_source_t source)) {
    s_on_preempted = on_preempted;

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

bool audio_source_claim(audio_source_t source) {
    static const char *const k_names[] = {
        [AUDIO_SOURCE_ESP32] = "A2DP", [AUDIO_SOURCE_AIRPLAY] = "AirPlay", [AUDIO_SOURCE_RADIO] = "Radio", [AUDIO_SOURCE_LOCAL] = "Ambience"};
    if (!take(source)) {
        return false;
    }
    ESP_LOGI(TAG, "%s has the DAC", k_names[source]);
    if (source != AUDIO_SOURCE_ESP32) {
        pause_a2dp();
    }
    return true;
}

static void pause_a2dp(void) {
    wireless_state_t esp32;
    if (uart_link_get_latest(&esp32) && esp32.connected && esp32.playing) {
        ESP_LOGI(TAG, "pausing A2DP");
        uart_link_send_command(CMD_PAUSE);
    }
}

void audio_source_release(audio_source_t source) {
    taskENTER_CRITICAL(&s_lock);
    if (!s_suspended && s_active == source) {
        select_locked(AUDIO_SOURCE_ESP32);
    }
    taskEXIT_CRITICAL(&s_lock);
}

void audio_source_hold_s3(bool hold) {
    taskENTER_CRITICAL(&s_lock);
    s_s3_held = hold;
    select_locked(s_active);
    taskEXIT_CRITICAL(&s_lock);
}

audio_source_t audio_source_active(void) {
    taskENTER_CRITICAL(&s_lock);
    audio_source_t active = s_active;
    taskEXIT_CRITICAL(&s_lock);
    return active;
}

void audio_source_set_local_preempted(void (*on_preempted)(void)) {
    s_on_local_preempted = on_preempted;
}

bool audio_source_claim_local(void) {
    return audio_source_claim(AUDIO_SOURCE_LOCAL);
}

void audio_source_release_local(void) {
    audio_source_release(AUDIO_SOURCE_LOCAL);
}

void audio_source_suspend(bool suspended) {
    taskENTER_CRITICAL(&s_lock);
    s_suspended = suspended;
    select_locked(suspended ? AUDIO_SOURCE_AIRPLAY : AUDIO_SOURCE_ESP32);
    taskEXIT_CRITICAL(&s_lock);
    if (suspended) {
        s_on_preempted(AUDIO_SOURCE_AIRPLAY);
        s_on_preempted(AUDIO_SOURCE_RADIO);
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
