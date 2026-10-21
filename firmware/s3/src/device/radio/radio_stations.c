#include "radio_stations.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define NVS_NAMESPACE "radio"
// The list as one blob: "name\0url\0" per station, in order.
#define NVS_KEY "stations"

static const char *TAG = "radio_stations";

// Public, unencrypted streams, one per codec the player takes.
static const radio_station_t k_samples[] = {
    {"SomaFM Groove Salad", "https://ice2.somafm.com/groovesalad-128-mp3"},
    {"Radio Paradise", "https://stream.radioparadise.com/aac-128"},
    {"SomaFM Drone Zone", "https://ice2.somafm.com/dronezone-128-aac"},
    {"SomaFM Secret Agent", "https://ice2.somafm.com/secretagent-128-mp3"},
};

static SemaphoreHandle_t s_lock;
static radio_station_t *s_list; // RADIO_STATIONS_MAX, in PSRAM
static int s_count;
static uint32_t s_version;

// Caller holds s_lock.
static void save_locked(void) {
    size_t cap = (size_t)s_count * sizeof(radio_station_t) + 1;
    char *blob = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!blob) {
        return;
    }
    size_t len = 0;
    for (int i = 0; i < s_count; i++) {
        len += (size_t)snprintf(blob + len, cap - len, "%s", s_list[i].name) + 1;
        len += (size_t)snprintf(blob + len, cap - len, "%s", s_list[i].url) + 1;
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        // An empty list is still a list: the samples don't come back.
        esp_err_t err = nvs_set_blob(nvs, NVS_KEY, blob, len ? len : 1);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "save failed: %s", esp_err_to_name(err));
        }
        nvs_close(nvs);
    }
    heap_caps_free(blob);
    s_version++;
}

static void parse(const char *blob, size_t len) {
    size_t at = 0;
    while (at < len && s_count < RADIO_STATIONS_MAX) {
        const char *name = blob + at;
        size_t name_len = strnlen(name, len - at);
        if (at + name_len + 1 >= len) {
            break;
        }
        const char *url = name + name_len + 1;
        size_t url_len = strnlen(url, len - at - name_len - 1);
        if (name_len && url_len) {
            snprintf(s_list[s_count].name, RADIO_NAME_MAX, "%s", name);
            snprintf(s_list[s_count].url, STREAM_URL_MAX, "%.*s", (int)url_len, url);
            s_count++;
        }
        at += name_len + 1 + url_len + 1;
    }
}

void radio_stations_init(void) {
    s_lock = xSemaphoreCreateMutex();
    s_list = heap_caps_calloc(RADIO_STATIONS_MAX, sizeof(radio_station_t), MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(s_lock && s_list ? ESP_OK : ESP_ERR_NO_MEM);

    nvs_handle_t nvs;
    size_t len = 0;
    bool stored = false;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        if (nvs_get_blob(nvs, NVS_KEY, NULL, &len) == ESP_OK) {
            char *blob = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
            if (blob && nvs_get_blob(nvs, NVS_KEY, blob, &len) == ESP_OK) {
                parse(blob, len);
                stored = true;
            }
            heap_caps_free(blob);
        }
        nvs_close(nvs);
    }
    if (!stored) {
        s_count = sizeof(k_samples) / sizeof(k_samples[0]);
        memcpy(s_list, k_samples, sizeof(k_samples));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        save_locked();
        xSemaphoreGive(s_lock);
    }
    ESP_LOGI(TAG, "%d station(s)", s_count);
}

int radio_stations_count(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int count = s_count;
    xSemaphoreGive(s_lock);
    return count;
}

bool radio_stations_get(int index, radio_station_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = index >= 0 && index < s_count;
    if (ok) {
        *out = s_list[index];
    }
    xSemaphoreGive(s_lock);
    return ok;
}

int radio_stations_find(const char *url) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int found = -1;
    for (int i = 0; i < s_count && found < 0; i++) {
        if (!strcmp(s_list[i].url, url)) {
            found = i;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

static bool station_valid(const char *name, const char *url) {
    bool scheme = !strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8);
    return name[0] && scheme && strlen(url) < STREAM_URL_MAX;
}

bool radio_stations_add(const char *name, const char *url) {
    if (!station_valid(name, url)) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = s_count < RADIO_STATIONS_MAX;
    if (ok) {
        radio_station_t *s = &s_list[s_count++];
        snprintf(s->name, sizeof(s->name), "%s", name);
        snprintf(s->url, sizeof(s->url), "%s", url);
        save_locked();
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool radio_stations_remove(int index) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = index >= 0 && index < s_count;
    if (ok) {
        memmove(&s_list[index], &s_list[index + 1], (size_t)(s_count - index - 1) * sizeof(radio_station_t));
        s_count--;
        save_locked();
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool radio_stations_update(int index, const char *name, const char *url) {
    if (!station_valid(name, url)) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = index >= 0 && index < s_count;
    if (ok) {
        snprintf(s_list[index].name, sizeof(s_list[index].name), "%s", name);
        snprintf(s_list[index].url, sizeof(s_list[index].url), "%s", url);
        save_locked();
    }
    xSemaphoreGive(s_lock);
    return ok;
}

bool radio_stations_move(int from, int to) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = from >= 0 && from < s_count && to >= 0 && to < s_count;
    if (ok && from != to) {
        radio_station_t moving = s_list[from];
        if (from < to) {
            memmove(&s_list[from], &s_list[from + 1], (size_t)(to - from) * sizeof(radio_station_t));
        } else {
            memmove(&s_list[to + 1], &s_list[to], (size_t)(from - to) * sizeof(radio_station_t));
        }
        s_list[to] = moving;
        save_locked();
    }
    xSemaphoreGive(s_lock);
    return ok;
}

uint32_t radio_stations_version(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t version = s_version;
    xSemaphoreGive(s_lock);
    return version;
}
