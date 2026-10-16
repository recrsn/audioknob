#include "airplay.h"

#include <stdio.h>
#include <string.h>

#include "airplay_player.h"
#include "artwork.h"
#include "audio_source.h"
#include "device_name.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "raop.h"
#include "sdkconfig.h"

#define CTRL_NET_UP BIT0
#define CTRL_VOLUME BIT1
#define CTRL_PLAY_PAUSE BIT2
#define CTRL_PAUSE BIT3
#define CTRL_NEXT BIT4
#define CTRL_PREV BIT5
#define CTRL_RENAME BIT6
#define CTRL_PROTOCOLS BIT7

// mbedtls key parsing in raop_create() plus blocking DACP HTTP.
#define CTRL_STACK_SIZE 6144
#define CTRL_TASK_PRIO 3
// A run of presses on the settings row lands as one change: mDNS needs a
// few seconds to probe and announce, and a change in the middle of that
// leaves records unannounced.
#define PROTOCOLS_SETTLE_US (2 * 1000 * 1000)

// Below everything audio: a large PNG cover takes seconds to decode.
#define ART_STACK_SIZE 6144
#define ART_TASK_PRIO 1

static const char *TAG = "airplay";

static SemaphoreHandle_t s_lock;
static wireless_state_t s_state = {.volume = 50, .source = SOURCE_AIRPLAY};
static track_info_t s_track;
static char s_client[PEER_NAME_MAX + 1]; // the session's sender
static artwork_slot_t *s_art;
static raop_ctx_t *s_raop;
static uint8_t s_mac[6];
static bool s_net_up;
static volatile uint8_t s_protocols;
static esp_timer_handle_t s_protocols_timer;
static TaskHandle_t s_ctrl_task;
static TaskHandle_t s_art_task;
// Latest cover not yet decoded (len 0 = clear); newer ones replace it.
static uint8_t *s_art_pending;
static size_t s_art_pending_len;
static bool s_art_has_pending;
// The last cover posted, so a sender repeating it (Music sends each one
// several times per track) doesn't decode it again.
static uint32_t s_art_hash;
static size_t s_art_len;
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

static uint32_t fnv1a(const uint8_t *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h = (h ^ data[i]) * 16777619u;
    }
    return h;
}

// Decoding on the RTSP thread would hold up its replies and starve CPU
// idle into the task watchdog, so covers go to the art task.
static void post_artwork(const uint8_t *data, size_t len) {
    uint32_t hash = fnv1a(data, len);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool repeat = len == s_art_len && hash == s_art_hash;
    s_art_len = len;
    s_art_hash = hash;
    xSemaphoreGive(s_lock);
    if (repeat) {
        return;
    }

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
    s_client[0] = '\0';
    xSemaphoreGive(s_lock);
    post_artwork(NULL, 0);
    audio_source_release_airplay();
}

static bool on_raop_event(const raop_event_t *event) {
    switch (event->type) {
    case RAOP_EVENT_SESSION_START:
        ESP_LOGI(TAG, "session start from \"%s\"", event->session.client ? event->session.client : "");
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_state.connected = true;
        s_state.playing = false;
        memset(&s_track, 0, sizeof(s_track));
        snprintf(s_client, sizeof(s_client), "%s", event->session.client ? event->session.client : "");
        xSemaphoreGive(s_lock);
        post_artwork(NULL, 0);
        break;

    case RAOP_EVENT_PLAY:
        if (!audio_source_claim_airplay()) {
            airplay_preempt();
            break;
        }
        set_playing(true);
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

    case RAOP_EVENT_METADATA: {
        track_info_t track = {0};
        snprintf(track.title, sizeof(track.title), "%s", event->metadata.title);
        snprintf(track.artist, sizeof(track.artist), "%s", event->metadata.artist);
        snprintf(track.album, sizeof(track.album), "%s", event->metadata.album);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        // Senders repeat it several times per track.
        bool changed = strcmp(track.title, s_track.title) || strcmp(track.artist, s_track.artist) ||
                       strcmp(track.album, s_track.album);
        if (changed) {
            strcpy(s_track.title, track.title);
            strcpy(s_track.artist, track.artist);
            strcpy(s_track.album, track.album);
        }
        xSemaphoreGive(s_lock);
        if (changed) {
            ESP_LOGI(TAG, "now playing: %s - %s", track.artist, track.title);
        }
        break;
    }

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

// Only once some AirPlay version is on, to keep its RAM free otherwise.
static void start_receiver(void) {
    char name[DEVICE_NAME_MAX + 1];
    device_name_get(name, sizeof(name));
    s_raop = raop_create(name, s_mac, s_protocols, on_raop_event, airplay_player_push);
    if (!s_raop) {
        ESP_LOGE(TAG, "AirPlay receiver failed to start");
    }
}

static void ctrl_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, UINT32_MAX, &bits, portMAX_DELAY);

        char name[DEVICE_NAME_MAX + 1];
        if ((bits & CTRL_NET_UP) && !s_net_up) {
            // mDNS also carries the hostname for the settings page.
            ESP_ERROR_CHECK(mdns_init());
            advertise_name(name);
            s_net_up = true;
        }
        if (!s_net_up) {
            continue;
        }
        if (bits & CTRL_RENAME) {
            advertise_name(name);
        }
        if (!s_raop) {
            if (s_protocols) {
                start_receiver();
            }
            continue;
        }

        if (bits & CTRL_RENAME) {
            raop_set_name(s_raop, name);
        }
        if (bits & CTRL_PROTOCOLS) {
            raop_set_protocols(s_raop, s_protocols);
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

static void protocols_settled(void *arg) {
    (void)arg;
    xTaskNotify(s_ctrl_task, CTRL_PROTOCOLS, eSetBits);
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
    const esp_timer_create_args_t timer = {.callback = protocols_settled, .name = "airplay_protocols"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &s_protocols_timer));
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

void airplay_get_client_name(char *out, size_t len) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out, len, "%s", s_client);
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

void airplay_set_protocols(bool airplay1, bool airplay2) {
    s_protocols = (airplay1 ? RAOP_PROTOCOL_AIRPLAY1 : 0) | (airplay2 ? RAOP_PROTOCOL_AIRPLAY2 : 0);
    esp_timer_stop(s_protocols_timer);
    esp_timer_start_once(s_protocols_timer, PROTOCOLS_SETTLE_US);
}

bool airplay_supports_airplay2(void) {
#if CONFIG_AK_AIRPLAY2
    return true;
#else
    return false;
#endif
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
