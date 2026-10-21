#include "online.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "settings.h"
#include "weather.h"

#define BODY_CAP           8192
#define WEATHER_REFRESH_S  (15 * 60)
#define RETRY_MIN_S        30
#define RETRY_MAX_S        (10 * 60)
// TLS handshakes run on this stack.
#define TASK_STACK         8192

#define NVS_NAMESPACE "location"

static const char *TAG = "online";

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_network_up;
static TaskHandle_t s_task;
// Guarded by s_lock. The detected location is cached in NVS so the
// timezone and forecast have a place before the network is up.
static struct {
    geo_t detected;
    bool have_detected;
    geo_t manual;
    bool have_manual;
    bool detected_dirty; // not yet written to NVS
    bool manual_dirty;
    uint32_t detect_requested;
    uint32_t detect_completed;
    uint32_t version;
} s_loc = {.version = 1};
static ui_weather_t s_weather;
static uint32_t s_weather_version;

static void load_blob(nvs_handle_t nvs, const char *key, geo_t *out, bool *have) {
    size_t len = sizeof(*out);
    *have = nvs_get_blob(nvs, key, out, &len) == ESP_OK && len == sizeof(*out);
}

static void load_cache(void) {
    settings_nvs_init();
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    load_blob(nvs, "detected", &s_loc.detected, &s_loc.have_detected);
    load_blob(nvs, "manual", &s_loc.manual, &s_loc.have_manual);
    nvs_close(nvs);
}

// NULL erases the key.
static bool save_blob(const char *key, const geo_t *geo) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return false;
    }
    esp_err_t err = geo ? nvs_set_blob(nvs, key, geo, sizeof(*geo)) : nvs_erase_key(nvs, key);
    if (!geo && err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err == ESP_OK;
}

// Flash writes stall both cores' caches, so the UI and web handlers leave
// them to this task. Retain failed writes and changes arriving during a write.
static bool save_cache(void) {
    bool saved = true;
    for (int i = 0; i < 2; i++) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool dirty = i ? s_loc.manual_dirty : s_loc.detected_dirty;
        bool have = i ? s_loc.have_manual : s_loc.have_detected;
        geo_t geo = i ? s_loc.manual : s_loc.detected;
        xSemaphoreGive(s_lock);
        if (!dirty) {
            continue;
        }
        if (!save_blob(i ? "manual" : "detected", have ? &geo : NULL)) {
            saved = false;
            continue;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool same = have == (i ? s_loc.have_manual : s_loc.have_detected) &&
                    (!have || !memcmp(&geo, i ? &s_loc.manual : &s_loc.detected, sizeof(geo)));
        if (same) {
            if (i) {
                s_loc.manual_dirty = false;
            } else {
                s_loc.detected_dirty = false;
            }
        } else {
            saved = false;
        }
        xSemaphoreGive(s_lock);
    }
    return saved;
}

static bool detection_needed(uint32_t *request) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *request = s_loc.detect_requested;
    bool needed = s_loc.detect_requested != s_loc.detect_completed ||
                  (!s_loc.have_manual && !s_loc.have_detected);
    xSemaphoreGive(s_lock);
    return needed;
}

static void set_detected(const geo_t *fresh, uint32_t request) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool moved = !s_loc.have_detected || memcmp(fresh, &s_loc.detected, sizeof(*fresh)) != 0;
    s_loc.detected = *fresh;
    s_loc.have_detected = true;
    s_loc.detect_completed = request;
    s_loc.detected_dirty |= moved;
    if (moved && !s_loc.have_manual) {
        s_loc.version++;
    }
    xSemaphoreGive(s_lock);
}

static void set_weather(const ui_weather_t *w) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_weather = *w;
    s_weather_version++;
    xSemaphoreGive(s_lock);
}

static void set_weather_status(const char *status) {
    ui_weather_t w = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // A failed refresh keeps the last good forecast on screen.
    bool keep = s_weather.valid;
    xSemaphoreGive(s_lock);
    if (!keep) {
        snprintf(w.status, sizeof(w.status), "%s", status);
        set_weather(&w);
    }
}

static int64_t now_s(void) {
    return xTaskGetTickCount() / configTICK_RATE_HZ;
}

// Sleeps until `s` seconds pass or the location is changed.
static void wait_s(int64_t s) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((s > 1 ? s : 1) * 1000));
}

static void online_task(void *arg) {
    (void)arg;
    // Response bodies are bulk data: PSRAM.
    char *buf = heap_caps_malloc(BODY_CAP, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "no memory for responses");
        vTaskDelete(NULL);
        return;
    }

    set_weather_status("Waiting for network");
    xSemaphoreTake(s_network_up, portMAX_DELAY);

    uint32_t weather_for = 0;
    uint32_t last_request = 0;
    uint32_t last_location = 0;
    int64_t geo_due = 0;
    int64_t weather_due = 0;
    int geo_retry_s = RETRY_MIN_S;
    int weather_retry_s = RETRY_MIN_S;

    for (;;) {
        int64_t now = now_s();
        uint32_t request;
        bool locate = detection_needed(&request);
        geo_t geo;
        uint32_t version = online_get_location(&geo, NULL);
        // A fresh Settings request (or returning to auto without a cache)
        // wakes a lookup immediately, including during a previous backoff.
        if (request != last_request || version != last_location) {
            geo_due = now;
            geo_retry_s = RETRY_MIN_S;
        }
        last_request = request;
        last_location = version;
        if (locate && now >= geo_due) {
            if (!version) {
                set_weather_status("Locating\xE2\x80\xA6");
            }
            geo_t fresh = {0};
            if (geo_lookup(&fresh, buf, BODY_CAP)) {
                set_detected(&fresh, request);
                geo_retry_s = RETRY_MIN_S;
                geo_due = 0;
            } else {
                set_weather_status("Can't find location");
                geo_due = now_s() + geo_retry_s;
                geo_retry_s = geo_retry_s * 2 > RETRY_MAX_S ? RETRY_MAX_S : geo_retry_s * 2;
            }
        }

        bool saved = save_cache();
        now = now_s();
        version = online_get_location(&geo, NULL);
        // Record the requested version even on failure, so another wakeup
        // does not bypass the weather retry deadline.
        if (version != weather_for) {
            weather_for = version;
            weather_due = now;
            weather_retry_s = RETRY_MIN_S;
        }
        if (version && now >= weather_due) {
            ui_weather_t w = {.valid = true};
            snprintf(w.place, sizeof(w.place), "%s", geo.city);
            if (weather_fetch(geo.lat, geo.lon, &w, buf, BODY_CAP)) {
                // Optional: retry air quality at the next weather refresh.
                air_quality_fetch(geo.lat, geo.lon, &w, buf, BODY_CAP);
                set_weather(&w);
                weather_due = now_s() + WEATHER_REFRESH_S;
                weather_retry_s = RETRY_MIN_S;
            } else {
                set_weather_status("Weather unavailable");
                weather_due = now_s() + weather_retry_s;
                weather_retry_s = weather_retry_s * 2 > RETRY_MAX_S ? RETRY_MAX_S : weather_retry_s * 2;
            }
        }

        // Without selected coordinates there is no weather deadline yet.
        int64_t next = version ? weather_due : now_s() + RETRY_MAX_S;
        if (detection_needed(&request) && geo_due < next) {
            next = geo_due;
        }
        if (!saved && now_s() + RETRY_MIN_S < next) {
            next = now_s() + RETRY_MIN_S;
        }
        wait_s(next - now_s());
    }
}

// Parsed JSON is many small nodes, which CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
// would otherwise put in the internal RAM AirPlay depends on.
static void *psram_malloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

void online_start(void) {
    cJSON_Hooks hooks = {.malloc_fn = psram_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    s_lock = xSemaphoreCreateMutex();
    s_network_up = xSemaphoreCreateBinary();
    load_cache();
    xTaskCreate(online_task, "online", TASK_STACK, NULL, 3, &s_task);
}

void online_network_up(void) {
    xSemaphoreGive(s_network_up);
}

uint32_t online_get_location(geo_t *out, bool *manual) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_loc.have_manual ? s_loc.manual : s_loc.detected;
    if (manual) {
        *manual = s_loc.have_manual;
    }
    // Back on automatic with nothing detected yet: no location at all.
    uint32_t v = s_loc.have_manual || s_loc.have_detected ? s_loc.version : 0;
    xSemaphoreGive(s_lock);
    return v;
}

void online_detect_location(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_loc.detect_requested++;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
}

void online_set_location(const geo_t *manual) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was = s_loc.have_manual;
    bool changed = manual ? !was || memcmp(manual, &s_loc.manual, sizeof(*manual)) != 0 : was;
    if (manual) {
        s_loc.manual = *manual;
    }
    s_loc.have_manual = manual != NULL;
    if (changed) {
        s_loc.manual_dirty = true;
        s_loc.version++;
    }
    xSemaphoreGive(s_lock);
    if (changed) {
        xTaskNotifyGive(s_task);
    }
}

uint32_t online_get_weather(ui_weather_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_weather;
    uint32_t v = s_weather_version;
    xSemaphoreGive(s_lock);
    return v;
}
