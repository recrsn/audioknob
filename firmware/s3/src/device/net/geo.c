#include "geo.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "http_get.h"

static const char *TAG = "geo";

static void copy_str(const cJSON *item, char *out, size_t len) {
    snprintf(out, len, "%s", cJSON_IsString(item) ? item->valuestring : "");
}

// {"success":true,"city":..,"latitude":..,"longitude":..,
//  "timezone":{"id":"Asia/Kolkata","offset":19800,..}}
static bool parse_ipwhois(const cJSON *root, geo_t *out) {
    const cJSON *tz = cJSON_GetObjectItem(root, "timezone");
    const cJSON *lat = cJSON_GetObjectItem(root, "latitude");
    const cJSON *lon = cJSON_GetObjectItem(root, "longitude");
    const cJSON *offset = cJSON_GetObjectItem(tz, "offset");
    if (!cJSON_IsTrue(cJSON_GetObjectItem(root, "success")) || !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon) ||
        !cJSON_IsNumber(offset)) {
        return false;
    }
    copy_str(cJSON_GetObjectItem(root, "city"), out->city, sizeof(out->city));
    copy_str(cJSON_GetObjectItem(tz, "id"), out->tz, sizeof(out->tz));
    out->lat = lat->valuedouble;
    out->lon = lon->valuedouble;
    out->tz_offset_s = offset->valueint;
    out->has_offset = true;
    return true;
}

// {"city":..,"loc":"12.97,77.59","timezone":"Asia/Kolkata",..}; no offset.
static bool parse_ipinfo(const cJSON *root, geo_t *out) {
    const cJSON *loc = cJSON_GetObjectItem(root, "loc");
    if (!cJSON_IsString(loc) || sscanf(loc->valuestring, "%lf,%lf", &out->lat, &out->lon) != 2) {
        return false;
    }
    copy_str(cJSON_GetObjectItem(root, "city"), out->city, sizeof(out->city));
    copy_str(cJSON_GetObjectItem(root, "timezone"), out->tz, sizeof(out->tz));
    out->has_offset = false;
    return true;
}

static const struct {
    const char *url;
    bool (*parse)(const cJSON *root, geo_t *out);
} k_providers[] = {
    {"https://ipwho.is/?fields=success,city,latitude,longitude,timezone", parse_ipwhois},
    {"https://ipinfo.io/json", parse_ipinfo},
};

bool geo_lookup(geo_t *out, char *buf, int cap) {
    for (size_t i = 0; i < sizeof(k_providers) / sizeof(k_providers[0]); i++) {
        if (http_get(k_providers[i].url, buf, cap) < 0) {
            continue;
        }
        cJSON *root = cJSON_Parse(buf);
        bool ok = root && k_providers[i].parse(root, out);
        cJSON_Delete(root);
        if (ok) {
            ESP_LOGI(TAG, "%s (%.2f, %.2f), %s", out->city, out->lat, out->lon, out->tz);
            return true;
        }
        ESP_LOGW(TAG, "unexpected response from %s", k_providers[i].url);
    }
    return false;
}
