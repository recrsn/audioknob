#include "radio/radio.h"

#include <stdio.h>
#include <string.h>

#include "audio/source.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "radio/stations.h"
#include "radio/stream/player.h"

#define NVS_NAMESPACE "radio"
#define DEFAULT_VOLUME 50
// Knob spins land in NVS as one write.
#define VOLUME_SAVE_US (2 * 1000 * 1000)

_Static_assert(RADIO_STATIONS_MAX == UI_RADIO_MAX && RADIO_NAME_MAX == UI_RADIO_NAME_MAX, "radio UI limits");

static const char *TAG = "radio";

static SemaphoreHandle_t s_lock;
static radio_station_t s_station; // the loaded one
static bool s_loaded;
static bool s_playing; // meant to be; the stream may still be connecting
static uint8_t s_volume = DEFAULT_VOLUME;
static uint32_t s_version;
static esp_timer_handle_t s_save_timer;

static void save_volume(void *arg) {
    (void)arg;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_u8(nvs, "volume", s_volume);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

void radio_init(void) {
    s_lock = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_lock ? ESP_OK : ESP_ERR_NO_MEM);
    radio_stations_init();
    stream_player_init();

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(nvs, "volume", &v) == ESP_OK && v <= 100) {
            s_volume = v;
        }
        nvs_close(nvs);
    }
    stream_player_set_volume(s_volume);
    const esp_timer_create_args_t timer = {.callback = save_volume, .name = "radio_volume"};
    ESP_ERROR_CHECK(esp_timer_create(&timer, &s_save_timer));
}

// Claims the DAC before the stream starts, so whatever held it stops first.
static void start(const radio_station_t *station) {
    if (!audio_source_claim(AUDIO_SOURCE_RADIO)) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_station = *station;
    s_loaded = true;
    s_playing = true;
    s_version++;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "playing \"%s\"", station->name);
    stream_player_play(station->url);
}

void radio_play(int index) {
    radio_station_t station;
    if (index < 0) {
        radio_stop();
    } else if (radio_stations_get(index, &station)) {
        start(&station);
    }
}

void radio_stop(void) {
    stream_player_stop();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_playing = false;
    s_version++;
    xSemaphoreGive(s_lock);
    audio_source_release(AUDIO_SOURCE_RADIO);
}

void radio_preempt(void) {
    stream_player_stop();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_loaded) {
        ESP_LOGI(TAG, "another source took the DAC");
    }
    s_playing = false;
    s_loaded = false;
    s_version++;
    xSemaphoreGive(s_lock);
}

void radio_toggle_play_pause(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool playing = s_playing, loaded = s_loaded;
    radio_station_t station = s_station;
    xSemaphoreGive(s_lock);
    if (playing) {
        radio_stop();
    } else if (loaded) {
        // Still playable when it has since been removed from the presets.
        start(&station);
    } else {
        radio_play(0);
    }
}

static void step_station(int delta) {
    int count = radio_stations_count();
    if (count == 0) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int at = s_loaded ? radio_stations_find(s_station.url) : -1;
    xSemaphoreGive(s_lock);
    int next = at < 0 ? (delta > 0 ? 0 : count - 1) : ((at + delta) % count + count) % count;
    radio_play(next);
}

void radio_next(void) {
    step_station(1);
}

void radio_prev(void) {
    step_station(-1);
}

void radio_adjust_volume(int steps) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int v = (int)s_volume + steps;
    s_volume = (uint8_t)(v < 0 ? 0 : v > 100 ? 100 : v);
    s_version++;
    uint8_t volume = s_volume;
    xSemaphoreGive(s_lock);
    stream_player_set_volume(volume);
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, VOLUME_SAVE_US);
}

bool radio_is_loaded(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool loaded = s_loaded;
    xSemaphoreGive(s_lock);
    return loaded;
}

static const char *activity(const stream_status_t *st) {
    switch (st->state) {
    case STREAM_CONNECTING:
        return "Connecting\xE2\x80\xA6";
    case STREAM_BUFFERING:
        return "Buffering\xE2\x80\xA6";
    case STREAM_RETRYING:
        return "Reconnecting\xE2\x80\xA6";
    case STREAM_FAILED:
        return st->error[0] ? st->error : "Can't play this station";
    default:
        return "";
    }
}

void radio_get_state(wireless_state_t *state, track_info_t *track) {
    stream_status_t st;
    stream_player_status(&st);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *state = (wireless_state_t){
        .volume = s_volume,
        .muted = s_volume == 0,
        .connected = s_loaded,
        // A station that gave up shows as stopped, ready to try again.
        .playing = s_playing && st.state != STREAM_FAILED,
        .source = SOURCE_RADIO,
    };
    memset(track, 0, sizeof(*track));
    if (s_loaded) {
        bool live_title = s_playing && st.state == STREAM_PLAYING && st.title[0];
        snprintf(track->title, sizeof(track->title), "%s", live_title ? st.title : s_station.name);
        snprintf(track->artist, sizeof(track->artist), "%s",
                 live_title ? s_station.name : s_playing ? activity(&st) : "");
    }
    xSemaphoreGive(s_lock);
}

void radio_get_station_name(char *out, size_t len) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out, len, "%s", s_loaded ? s_station.name : "");
    xSemaphoreGive(s_lock);
}

uint32_t radio_ui_snapshot(ui_radio_t *out) {
    static const ui_radio_state_t k_states[] = {
        [STREAM_IDLE] = UI_RADIO_STOPPED,         [STREAM_CONNECTING] = UI_RADIO_CONNECTING,
        [STREAM_BUFFERING] = UI_RADIO_BUFFERING,  [STREAM_PLAYING] = UI_RADIO_PLAYING,
        [STREAM_RETRYING] = UI_RADIO_RETRYING,    [STREAM_FAILED] = UI_RADIO_FAILED,
    };
    stream_status_t st;
    uint32_t version = stream_player_status(&st);

    memset(out, 0, sizeof(*out));
    out->count = radio_stations_count();
    for (int i = 0; i < out->count; i++) {
        radio_station_t station;
        if (radio_stations_get(i, &station)) {
            memcpy(out->names[i], station.name, sizeof(out->names[i]));
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->current = s_loaded ? radio_stations_find(s_station.url) : -1;
    out->state = s_playing ? k_states[st.state] : UI_RADIO_STOPPED;
    if (s_playing) {
        snprintf(out->detail, sizeof(out->detail), "%s",
                 st.state == STREAM_PLAYING ? st.title : st.state == STREAM_FAILED || st.state == STREAM_RETRYING ? st.error : "");
    }
    version += s_version * 7919u + radio_stations_version() * 104729u;
    xSemaphoreGive(s_lock);
    return version;
}
