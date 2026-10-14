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
#include "weather.h"

#define BODY_CAP           8192
#define LOCATION_REFRESH_S (12 * 3600)
#define WEATHER_REFRESH_S  (15 * 60)
#define RETRY_MIN_S        30
#define RETRY_MAX_S        (10 * 60)
// TLS handshakes run on this stack.
#define TASK_STACK         8192

static const char *TAG = "online";

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_network_up;
static geo_t s_geo;
static uint32_t s_geo_version;
static ui_weather_t s_weather;
static uint32_t s_weather_version;

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

    geo_t geo;
    bool have_geo = false;
    int64_t geo_due = 0;
    int64_t weather_due = 0;
    int retry_s = RETRY_MIN_S;

    for (;;) {
        int64_t now = now_s();
        bool failed = false;

        if (now >= geo_due) {
            if (!have_geo) {
                set_weather_status("Locating\xE2\x80\xA6");
            }
            geo_t fresh = {0};
            if (geo_lookup(&fresh, buf, BODY_CAP)) {
                xSemaphoreTake(s_lock, portMAX_DELAY);
                bool moved = !have_geo || memcmp(&fresh, &s_geo, sizeof(fresh)) != 0;
                s_geo = fresh;
                if (moved) {
                    s_geo_version++;
                }
                xSemaphoreGive(s_lock);
                geo = fresh;
                weather_due = moved ? now : weather_due;
                have_geo = true;
                geo_due = now + LOCATION_REFRESH_S;
            } else if (have_geo) {
                // Keep the old location; try again on the normal cadence.
                geo_due = now + LOCATION_REFRESH_S;
            } else {
                set_weather_status("Can't find location");
                failed = true;
            }
        }

        if (have_geo && now >= weather_due) {
            ui_weather_t w = {.valid = true};
            snprintf(w.place, sizeof(w.place), "%s", geo.city);
            if (weather_fetch(geo.lat, geo.lon, &w, buf, BODY_CAP)) {
                set_weather(&w);
                weather_due = now + WEATHER_REFRESH_S;
            } else {
                set_weather_status("Weather unavailable");
                failed = true;
            }
        }

        if (failed) {
            vTaskDelay(pdMS_TO_TICKS(retry_s * 1000));
            retry_s = retry_s * 2 > RETRY_MAX_S ? RETRY_MAX_S : retry_s * 2;
            continue;
        }
        retry_s = RETRY_MIN_S;
        int64_t next = have_geo && weather_due < geo_due ? weather_due : geo_due;
        int64_t wait = next - now_s();
        vTaskDelay(pdMS_TO_TICKS((wait > 1 ? wait : 1) * 1000));
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
    xTaskCreate(online_task, "online", TASK_STACK, NULL, 3, NULL);
}

void online_network_up(void) {
    xSemaphoreGive(s_network_up);
}

uint32_t online_get_location(geo_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_geo;
    uint32_t v = s_geo_version;
    xSemaphoreGive(s_lock);
    return v;
}

uint32_t online_get_weather(ui_weather_t *out) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_weather;
    uint32_t v = s_weather_version;
    xSemaphoreGive(s_lock);
    return v;
}
