#include "airplay.h"

#include <stdio.h>
#include <string.h>

#include "airplay_player.h"
#include "artwork.h"
#include "audio_source.h"
#include "device_name.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "raop.h"

#define CTRL_NET_UP BIT0
#define CTRL_VOLUME BIT1
#define CTRL_PLAY_PAUSE BIT2
#define CTRL_PAUSE BIT3
#define CTRL_NEXT BIT4
#define CTRL_PREV BIT5
#define CTRL_RENAME BIT6

// mbedtls key parsing in raop_create() plus blocking DACP HTTP.
#define CTRL_STACK_SIZE 6144
#define CTRL_TASK_PRIO 3

// Below everything audio: a large PNG cover takes seconds to decode.
#define ART_STACK_SIZE 6144
#define ART_TASK_PRIO 1

static const char *TAG = "airplay";

static SemaphoreHandle_t s_lock;
static wireless_state_t s_state = {.volume = 50, .source = SOURCE_AIRPLAY};
static track_info_t s_track;
static artwork_slot_t *s_art;
static raop_ctx_t *s_raop;
static uint8_t s_mac[6];
static TaskHandle_t s_ctrl_task;
static TaskHandle_t s_art_task;
// Latest cover not yet decoded (len 0 = clear); newer ones replace it.
static uint8_t *s_art_pending;
static size_t s_art_pending_len;
static bool s_art_has_pending;
// Latest knob volume not yet pushed to the sender; coalesces fast spins.
static volatile uint8_t s_pending_volume;

static void set_playing(bool playing) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.playing = playing;
    xSemaphoreGive(s_lock);
}

static void set_volume(uint8_t volume) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.volume = volume;
    s_state.muted = volume == 0;
    xSemaphoreGive(s_lock);
    airplay_player_set_volume(volume);
}

// Decoding on the RTSP thread would hold up its replies and starve CPU
// idle into the task watchdog, so covers go to the art task.
static void post_artwork(const uint8_t *data, size_t len) {
    uint8_t *copy = NULL;
    if (len) {
        copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (!copy) {
            ESP_LOGW(TAG, "no memory for %u byte cover", (unsigned)len);
            len = 0;
        } else {
            memcpy(copy, data, len);
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    heap_caps_free(s_art_pending);
    s_art_pending = copy;
    s_art_pending_len = len;
    s_art_has_pending = true;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_art_task);
}

static void art_task(void *arg) {
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool has = s_art_has_pending;
        uint8_t *data = s_art_pending;
        size_t len = s_art_pending_len;
        s_art_pending = NULL;
        s_art_has_pending = false;
        xSemaphoreGive(s_lock);
        if (has) {
            artwork_slot_set(s_art, data, len);
            heap_caps_free(data);
        }
    }
}

static void end_session(void) {
    airplay_player_flush();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.connected = false;
    s_state.playing = false;
    memset(&s_track, 0, sizeof(s_track));
    xSemaphoreGive(s_lock);
    post_artwork(NULL, 0);
    audio_source_release_airplay();
}

static bool on_raop_event(const raop_event_t *event) {
    switch (event->type) {
    case RAOP_EVENT_SESSION_START:
        ESP_LOGI(TAG, "session start");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state.connected = true;
        s_state.playing = false;
        memset(&s_track, 0, sizeof(s_track));
        xSemaphoreGive(s_lock);
        post_artwork(NULL, 0);
        break;

    case RAOP_EVENT_PLAY:
        set_playing(true);
        audio_source_claim_airplay();
        break;

    case RAOP_EVENT_FLUSH:
        // Pause or seek. Hand the DAC back so a paused AirPlay session
        // never blocks A2DP; the next PLAY claims it again.
        airplay_player_flush();
        set_playing(false);
        audio_source_release_airplay();
        break;

    case RAOP_EVENT_SESSION_END:
        ESP_LOGI(TAG, "session end");
        end_session();
        break;

    case RAOP_EVENT_STALLED:
        ESP_LOGW(TAG, "sender stalled, dropping session");
        raop_abort(s_raop);
        break;

    case RAOP_EVENT_VOLUME:
        set_volume((uint8_t)(event->volume * 100.0f + 0.5f));
        break;

    case RAOP_EVENT_METADATA:
        xSemaphoreTake(s_lock, portMAX_DELAY);
        snprintf(s_track.title, sizeof(s_track.title), "%s", event->metadata.title);
        snprintf(s_track.artist, sizeof(s_track.artist), "%s", event->metadata.artist);
        snprintf(s_track.album, sizeof(s_track.album), "%s", event->metadata.album);
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "now playing: %s - %s", event->metadata.artist, event->metadata.title);
        break;

    case RAOP_EVENT_ARTWORK:
        post_artwork(event->artwork.data, event->artwork.len);
        break;
    }
    return true;
}

// Hostname and default instance name follow the device name, so the
// settings page is at http://<hostname>.local/.
static void advertise_name(char name[DEVICE_NAME_MAX + 1]) {
    char hostname[64];
    device_name_get(name, DEVICE_NAME_MAX + 1);
    device_name_hostname(hostname, sizeof(hostname));
    mdns_hostname_set(hostname);
    mdns_instance_name_set(name);
}

static void start_receiver(void) {
    ESP_ERROR_CHECK(mdns_init());
    char name[DEVICE_NAME_MAX + 1];
    advertise_name(name);

    s_raop = raop_create(name, s_mac, on_raop_event, airplay_player_push);
    if (!s_raop) {
        ESP_LOGE(TAG, "AirPlay receiver failed to start");
    }
}

static void ctrl_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);

        if ((bits & CTRL_NET_UP) && !s_raop) {
            start_receiver();
        }
        if (!s_raop) {
            continue;
        }

        if (bits & CTRL_RENAME) {
            char name[DEVICE_NAME_MAX + 1];
            advertise_name(name);
            raop_set_name(s_raop, name);
        }

        if ((bits & CTRL_PAUSE) && !raop_remote_command(s_raop, "pause")) {
            // No way to ask the sender to stop: drop it rather than leave it
            // streaming into a DAC it no longer owns.
            raop_abort(s_raop);
        }
        if (bits & CTRL_PLAY_PAUSE) {
            raop_remote_command(s_raop, "playpause");
        }
        if (bits & CTRL_NEXT) {
            raop_remote_command(s_raop, "nextitem");
        }
        if (bits & CTRL_PREV) {
            raop_remote_command(s_raop, "previtem");
        }
        if (bits & CTRL_VOLUME) {
            raop_remote_volume(s_raop, s_pending_volume / 100.0f);
        }
    }
}

void airplay_init(void) {
    s_lock = xSemaphoreCreateMutex();
    s_art = artwork_slot_create();
    airplay_player_init();
    airplay_player_set_volume(s_state.volume);

    BaseType_t ok =
        xTaskCreatePinnedToCore(ctrl_task, "airplay_ctrl", CTRL_STACK_SIZE, NULL, CTRL_TASK_PRIO, &s_ctrl_task, 0);
    BaseType_t art_ok = xTaskCreatePinnedToCore(art_task, "airplay_art", ART_STACK_SIZE, NULL, ART_TASK_PRIO,
                                                &s_art_task, tskNO_AFFINITY);
    ESP_ERROR_CHECK(s_lock && s_art && ok == pdPASS && art_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}

void airplay_network_up(const uint8_t mac[6]) {
    memcpy(s_mac, mac, sizeof(s_mac));
    xTaskNotify(s_ctrl_task, CTRL_NET_UP, eSetBits);
}

bool airplay_is_connected(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool connected = s_state.connected;
    xSemaphoreGive(s_lock);
    return connected;
}

void airplay_get_state(wireless_state_t *state, track_info_t *track) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *state = s_state;
    *track = s_track;
    xSemaphoreGive(s_lock);
}

const uint16_t *airplay_get_artwork(uint32_t *version) {
    return artwork_slot_get(s_art, version);
}

void airplay_adjust_volume(int steps) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int volume = (int)s_state.volume + steps;
    xSemaphoreGive(s_lock);

    volume = volume < 0 ? 0 : (volume > 100 ? 100 : volume);
    set_volume((uint8_t)volume);
    s_pending_volume = (uint8_t)volume;
    xTaskNotify(s_ctrl_task, CTRL_VOLUME, eSetBits);
}

void airplay_rename(void) {
    xTaskNotify(s_ctrl_task, CTRL_RENAME, eSetBits);
}

void airplay_toggle_play_pause(void) {
    xTaskNotify(s_ctrl_task, CTRL_PLAY_PAUSE, eSetBits);
}

void airplay_next(void) {
    xTaskNotify(s_ctrl_task, CTRL_NEXT, eSetBits);
}

void airplay_prev(void) {
    xTaskNotify(s_ctrl_task, CTRL_PREV, eSetBits);
}

void airplay_preempt(void) {
    airplay_player_flush();
    set_playing(false);
    xTaskNotify(s_ctrl_task, CTRL_PAUSE, eSetBits);
}
