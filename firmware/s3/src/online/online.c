#include "online/online.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "net/network_jobs.h"
#include "nvs.h"
#include "core/settings/settings.h"
#include "online/weather.h"

#define BODY_CAP           8192
#define WEATHER_REFRESH_S  (15 * 60)
#define RETRY_MIN_S        30
#define RETRY_MAX_S        (10 * 60)

#define NVS_NAMESPACE "location"

static const char *TAG = "online";

static SemaphoreHandle_t s_lock;
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
// Only the shared worker changes these retry counters.
static uint32_t s_last_geo_request;
static uint32_t s_weather_for;
static int s_geo_retry_s = RETRY_MIN_S;
static int s_weather_retry_s = RETRY_MIN_S;

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

static int next_retry(int retry) {
    return retry * 2 > RETRY_MAX_S ? RETRY_MAX_S : retry * 2;
}

static void save_job(void) {
    if (!save_cache()) {
        network_jobs_schedule(NETWORK_JOB_LOCATION_SAVE, RETRY_MIN_S);
    }
}

static void locate_job(void) {
    uint32_t request;
    if (!detection_needed(&request)) {
        return;
    }
    if (request != s_last_geo_request) {
        s_geo_retry_s = RETRY_MIN_S;
        s_last_geo_request = request;
    }
    geo_t geo;
    uint32_t before = online_get_location(&geo, NULL);
    if (!before) {
        set_weather_status("Locating\xE2\x80\xA6");
    }
    char *buf = heap_caps_malloc(BODY_CAP, MALLOC_CAP_SPIRAM);
    geo_t fresh = {0};
    bool ok = buf && geo_lookup(&fresh, buf, BODY_CAP);
    free(buf);
    if (!ok) {
        set_weather_status("Can't find location");
        network_jobs_schedule(NETWORK_JOB_LOCATION, s_geo_retry_s);
        s_geo_retry_s = next_retry(s_geo_retry_s);
        return;
    }
    set_detected(&fresh, request);
    s_geo_retry_s = RETRY_MIN_S;
    network_jobs_schedule(NETWORK_JOB_LOCATION_SAVE, 0);
    if (online_get_location(&geo, NULL) != before) {
        network_jobs_schedule(NETWORK_JOB_WEATHER, 0);
    }
    // A request made during the blocking lookup must survive its completion.
    if (detection_needed(&request)) {
        network_jobs_schedule(NETWORK_JOB_LOCATION, 0);
    }
}

static void weather_job(void) {
    geo_t geo;
    uint32_t version = online_get_location(&geo, NULL);
    if (!version) {
        return; // Successful location discovery will schedule weather.
    }
    if (version != s_weather_for) {
        s_weather_for = version;
        s_weather_retry_s = RETRY_MIN_S;
    }
    char *buf = heap_caps_malloc(BODY_CAP, MALLOC_CAP_SPIRAM);
    ui_weather_t w = {.valid = true};
    snprintf(w.place, sizeof(w.place), "%s", geo.city);
    bool ok = buf && weather_fetch(geo.lat, geo.lon, &w, buf, BODY_CAP);
    if (ok) {
        // Optional; a failure retries with the next forecast, not a hot loop.
        air_quality_fetch(geo.lat, geo.lon, &w, buf, BODY_CAP);
    }
    free(buf);
    geo_t current;
    if (online_get_location(&current, NULL) != version) {
        network_jobs_schedule(NETWORK_JOB_WEATHER, 0);
        return; // Do not publish a forecast for an obsolete manual selection.
    }
    if (ok) {
        set_weather(&w);
        s_weather_retry_s = RETRY_MIN_S;
        network_jobs_schedule(NETWORK_JOB_WEATHER, WEATHER_REFRESH_S);
    } else {
        set_weather_status("Weather unavailable");
        network_jobs_schedule(NETWORK_JOB_WEATHER, s_weather_retry_s);
        s_weather_retry_s = next_retry(s_weather_retry_s);
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
    load_cache();
    network_jobs_init();
    network_jobs_register(NETWORK_JOB_LOCATION, locate_job, true);
    network_jobs_register(NETWORK_JOB_WEATHER, weather_job, true);
    network_jobs_register(NETWORK_JOB_LOCATION_SAVE, save_job, false);
    set_weather_status("Waiting for network");
    uint32_t request;
    if (detection_needed(&request)) {
        network_jobs_schedule(NETWORK_JOB_LOCATION, 0);
    }
    network_jobs_schedule(NETWORK_JOB_WEATHER, 0);
}

void online_network_up(void) {
    network_jobs_set_network(true);
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
    network_jobs_schedule(NETWORK_JOB_LOCATION, 0);
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
        network_jobs_schedule(NETWORK_JOB_LOCATION_SAVE, 0);
        network_jobs_schedule(NETWORK_JOB_LOCATION, 0);
        network_jobs_schedule(NETWORK_JOB_WEATHER, 0);
    }
}

uint32_t online_get_weather(ui_weather_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_weather;
    uint32_t v = s_weather_version;
    xSemaphoreGive(s_lock);
    return v;
}
