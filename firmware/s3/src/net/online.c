#ifndef AK_NATIVE

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
#define LOCATION_REFRESH_S (12 * 3600)
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
    bool manual_dirty; // not yet written to NVS
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
static void save_blob(const char *key, const geo_t *geo) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "can't open NVS");
        return;
    }
    if (geo) {
        nvs_set_blob(nvs, key, geo, sizeof(*geo));
    } else {
        nvs_erase_key(nvs, key);
    }
    nvs_commit(nvs);
    nvs_close(nvs);
}

// Flash writes stall both cores' caches, so the UI and web handlers leave
// them to this task.
static void save_manual_if_dirty(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool dirty = s_loc.manual_dirty;
    bool have = s_loc.have_manual;
    geo_t manual = s_loc.manual;
    s_loc.manual_dirty = false;
    xSemaphoreGive(s_lock);
    if (dirty) {
        save_blob("manual", have ? &manual : NULL);
    }
}

static bool manual_active(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool manual = s_loc.have_manual;
    xSemaphoreGive(s_lock);
    return manual;
}

static void set_detected(const geo_t *fresh) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool moved = !s_loc.have_detected || memcmp(fresh, &s_loc.detected, sizeof(*fresh)) != 0;
    s_loc.detected = *fresh;
    s_loc.have_detected = true;
    if (moved && !s_loc.have_manual) {
        s_loc.version++;
    }
    xSemaphoreGive(s_lock);
    if (moved) {
        save_blob("detected", fresh);
    }
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

    bool located = false; // by IP since boot, not just from the cache
    uint32_t weather_for = 0; // location version the forecast is for
    int64_t geo_due = 0;
    int64_t weather_due = 0;
    int retry_s = RETRY_MIN_S;

    for (;;) {
        save_manual_if_dirty();
        int64_t now = now_s();
        bool failed = false;
        bool manual = manual_active();
        geo_t geo;

        // A manual location needs no lookup; a cached one is refreshed.
        if (!manual && now >= geo_due) {
            bool known = online_get_location(&geo, NULL) != 0;
            if (!known) {
                set_weather_status("Locating\xE2\x80\xA6");
            }
            geo_t fresh = {0};
            if (geo_lookup(&fresh, buf, BODY_CAP)) {
                set_detected(&fresh);
                located = true;
                geo_due = now + LOCATION_REFRESH_S;
            } else if (known) {
                // Keep the old location; a cached one is retried sooner.
                geo_due = now + (located ? LOCATION_REFRESH_S : RETRY_MAX_S);
            } else {
                set_weather_status("Can't find location");
                failed = true;
            }
        }

        uint32_t version = online_get_location(&geo, NULL);
        if (version != weather_for) {
            weather_due = now;
        }
        if (version && now >= weather_due) {
            ui_weather_t w = {.valid = true};
            snprintf(w.place, sizeof(w.place), "%s", geo.city);
            if (weather_fetch(geo.lat, geo.lon, &w, buf, BODY_CAP)) {
                set_weather(&w);
                weather_for = version;
                weather_due = now + WEATHER_REFRESH_S;
            } else {
                set_weather_status("Weather unavailable");
                failed = true;
            }
        }

        if (failed) {
            wait_s(retry_s);
            retry_s = retry_s * 2 > RETRY_MAX_S ? RETRY_MAX_S : retry_s * 2;
            continue;
        }
        retry_s = RETRY_MIN_S;
        int64_t next = weather_due;
        if (!manual_active() && geo_due < next) {
            next = geo_due;
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

#endif  // !AK_NATIVE
