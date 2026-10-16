#ifndef AK_NATIVE

#include "weather.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "http_get.h"

// Hourly data starts at an hour boundary before now, so ask for a few more
// hours than shown and skip the ones already past.
#define FORECAST_HOURS (UI_WEATHER_HOURS + 4)

static const char *TAG = "weather";

static double number_at(const cJSON *array, int i) {
    const cJSON *item = cJSON_GetArrayItem(array, i);
    return cJSON_IsNumber(item) ? item->valuedouble : 0;
}

static double number_of(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItem(object, key);
    return cJSON_IsNumber(item) ? item->valuedouble : 0;
}

static bool parse(const cJSON *root, ui_weather_t *out) {
    const cJSON *current = cJSON_GetObjectItem(root, "current");
    const cJSON *now = cJSON_GetObjectItem(current, "time");
    const cJSON *temp = cJSON_GetObjectItem(current, "temperature_2m");
    const cJSON *code = cJSON_GetObjectItem(current, "weather_code");
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    const cJSON *hourly = cJSON_GetObjectItem(root, "hourly");
    const cJSON *times = cJSON_GetObjectItem(hourly, "time");
    if (!cJSON_IsString(now) || !cJSON_IsNumber(temp) || !cJSON_IsNumber(code) || !cJSON_IsArray(times)) {
        return false;
    }

    out->temp_c = (float)temp->valuedouble;
    out->code = (uint8_t)code->valueint;
    out->is_day = number_of(current, "is_day") != 0;
    out->high_c = (float)number_at(cJSON_GetObjectItem(daily, "temperature_2m_max"), 0);
    out->low_c = (float)number_at(cJSON_GetObjectItem(daily, "temperature_2m_min"), 0);

    // Times are local ISO 8601 ("2026-10-06T21:00"), so they compare as
    // strings and the hour is at a fixed offset.
    const cJSON *temps = cJSON_GetObjectItem(hourly, "temperature_2m");
    const cJSON *codes = cJSON_GetObjectItem(hourly, "weather_code");
    const cJSON *days = cJSON_GetObjectItem(hourly, "is_day");
    int slot = 0;
    for (int i = 0; i < cJSON_GetArraySize(times) && slot < UI_WEATHER_HOURS; i++) {
        const char *t = cJSON_GetArrayItem(times, i)->valuestring;
        if (!t || strlen(t) < 13 || strcmp(t, now->valuestring) <= 0) {
            continue;
        }
        out->hours[slot].hour = (uint8_t)((t[11] - '0') * 10 + (t[12] - '0'));
        out->hours[slot].temp_c = (float)number_at(temps, i);
        out->hours[slot].code = (uint8_t)number_at(codes, i);
        out->hours[slot].is_day = number_at(days, i) != 0;
        slot++;
    }
    return slot == UI_WEATHER_HOURS;
}

bool weather_fetch(double lat, double lon, ui_weather_t *out, char *buf, int cap) {
    char url[384];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m,weather_code,is_day&hourly=temperature_2m,weather_code,is_day"
             "&daily=temperature_2m_max,temperature_2m_min&timezone=auto&forecast_days=2&forecast_hours=%d",
             lat, lon, FORECAST_HOURS);
    if (http_get(url, buf, cap) < 0) {
        return false;
    }
    cJSON *root = cJSON_Parse(buf);
    bool ok = root && parse(root, out);
    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGW(TAG, "unexpected forecast response");
    }
    return ok;
}

#endif  // !AK_NATIVE
