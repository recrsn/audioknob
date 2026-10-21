#include "web/api/api.h"

#include <stdio.h>
#include <string.h>
#include "airplay/airplay.h"
#include "cJSON.h"
#include "core/device_name.h"
#include "core/now_playing.h"
#include "core/settings/ui.h"
#include "ambience/focus_sound.h"
#include "drivers/uart_link.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "online/online.h"
#include "radio/radio.h"
#include "radio/stations.h"
#include "ui/apps/settings/tz.h"
#include "update/github_release.h"
#include "update/ota.h"
#include "web/page.h"

#define ZONES_CHUNK 1024
// Lets the reply reach the browser before the device restarts.
#define FORGET_DELAY_MS 300

static cJSON *now_playing_json(void) {
    cJSON *np = cJSON_CreateObject();
    wireless_state_t state = {0};
    track_info_t track;
    bool known = now_playing_get(&state, &track);
    uint32_t art_version = 0;
    if (!now_playing_artwork(&art_version)) {
        art_version = 0;
    }
    cJSON_AddStringToObject(np, "source", state.source == SOURCE_A2DP    ? "bluetooth"
                                          : state.source == SOURCE_RADIO ? "radio"
                                                                         : "airplay");
    cJSON_AddBoolToObject(np, "connected", known && state.connected);
    cJSON_AddBoolToObject(np, "playing", known && state.playing);
    cJSON_AddNumberToObject(np, "volume", state.volume);
    cJSON_AddBoolToObject(np, "muted", state.muted);
    cJSON_AddStringToObject(np, "title", track.title);
    cJSON_AddStringToObject(np, "artist", track.artist);
    cJSON_AddStringToObject(np, "album", track.album);
    cJSON_AddNumberToObject(np, "art", art_version);
    const char *format = state.source == SOURCE_AIRPLAY ? airplay_stream_format() : NULL;
    if (format) {
        cJSON_AddStringToObject(np, "format", format);
    }
    return np;
}

static cJSON *update_json(void) {
    static const char *const k_states[] = {
        [OTA_IDLE] = "idle",         [OTA_RECEIVING] = "receiving",   [OTA_READY] = "ready",
        [OTA_DECLINED] = "declined", [OTA_INSTALLING] = "installing", [OTA_INSTALLED] = "installed",
        [OTA_FAILED] = "failed",
    };
    ota_status_t ota;
    ota_get_status(&ota);
    cJSON *update = cJSON_CreateObject();
    cJSON_AddStringToObject(update, "state", k_states[ota.state]);
    cJSON_AddStringToObject(update, "target", ota.target == OTA_TARGET_ESP32 ? "bluetooth" : "s3");
    cJSON_AddNumberToObject(update, "progress", ota.progress);
    cJSON_AddStringToObject(update, "version", ota.version);
    cJSON_AddStringToObject(update, "error", ota.error);
    return update;
}

// Celsius; the page converts per the units setting. Missing values are null.
static cJSON *weather_json(void) {
    ui_weather_t w;
    if (!online_get_weather(&w) || !w.valid) {
        return cJSON_CreateNull();
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "place", w.place);
    cJSON_AddNumberToObject(json, "tempC", w.temp_c);
    if (w.has_aqi) {
        cJSON_AddNumberToObject(json, "aqi", w.aqi);
        cJSON_AddNumberToObject(json, "pm25", w.pm2_5);
    }
    if (w.has_uv) {
        cJSON_AddNumberToObject(json, "uv", w.uv);
        cJSON_AddNumberToObject(json, "uvMax", w.uv_max);
    }
    return json;
}

static cJSON *radio_json(void) {
    static const char *const k_states[] = {
        [UI_RADIO_STOPPED] = "stopped",     [UI_RADIO_CONNECTING] = "connecting", [UI_RADIO_BUFFERING] = "buffering",
        [UI_RADIO_PLAYING] = "playing",     [UI_RADIO_RETRYING] = "retrying",     [UI_RADIO_FAILED] = "failed",
    };
    ui_radio_t *r = heap_caps_malloc(sizeof(*r), MALLOC_CAP_SPIRAM);
    cJSON *radio = cJSON_CreateObject();
    if (r) {
        radio_ui_snapshot(r);
        cJSON_AddNumberToObject(radio, "current", r->current);
        cJSON_AddStringToObject(radio, "state", k_states[r->state]);
        cJSON_AddStringToObject(radio, "detail", r->detail);
        heap_caps_free(r);
    }
    // Tells the page when to fetch /api/stations again.
    cJSON_AddNumberToObject(radio, "list", radio_stations_version());
    return radio;
}

esp_err_t web_api_state(httpd_req_t *req) {
    ui_settings_t s;
    settings_ui_snapshot(&s);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "name", s.device_name);
    cJSON_AddStringToObject(json, "hostname", s.hostname);
    cJSON_AddStringToObject(json, "ssid", s.wifi_ssid);
    cJSON_AddStringToObject(json, "ip", s.ip);
    cJSON_AddStringToObject(json, "version", s.version);
    hello_t esp32;
    uart_link_get_esp32_firmware(&esp32);
    cJSON_AddStringToObject(json, "btVersion", esp32.version);
    cJSON_AddNumberToObject(json, "freeInternalKb", s.free_internal_kb);
    cJSON_AddNumberToObject(json, "freePsramKb", s.free_psram_kb);
    cJSON_AddNumberToObject(json, "storageTotalBytes", s.storage_total_bytes);
    cJSON_AddNumberToObject(json, "storageUsedBytes", s.storage_used_bytes);
    cJSON_AddNumberToObject(json, "brightness", s.brightness);
    cJSON_AddNumberToObject(json, "sleep", s.sleep_s);
    cJSON_AddBoolToObject(json, "clock24", s.clock_24h);
    cJSON_AddBoolToObject(json, "fahrenheit", s.fahrenheit);
    cJSON_AddBoolToObject(json, "haptics", s.haptics);
    cJSON_AddBoolToObject(json, "artColors", s.art_colors);
    cJSON_AddBoolToObject(json, "airplayLossless", s.airplay_lossless);
    cJSON_AddNumberToObject(json, "timerSound", s.timer_sound);
    cJSON_AddNumberToObject(json, "timerVolume", s.timer_volume);
    cJSON_AddBoolToObject(json, "bluetooth", s.bluetooth);
    cJSON_AddNumberToObject(json, "airShow", s.air_show);
    cJSON_AddNumberToObject(json, "ambienceSound", s.ambience_sound);
    cJSON_AddNumberToObject(json, "ambienceVolume", s.ambience_volume);
    cJSON_AddNumberToObject(json, "focusSound", s.focus_sound);
    cJSON_AddBoolToObject(json, "tzAuto", s.tz_auto);
    cJSON_AddStringToObject(json, "tz", s.tz_name);
    cJSON_AddBoolToObject(json, "locationAuto", s.location_auto);
    cJSON_AddStringToObject(json, "location", s.location);
    cJSON_AddItemToObject(json, "np", now_playing_json());
    cJSON_AddItemToObject(json, "weather", weather_json());
    ambience_kind_t kind;
    cJSON *amb = cJSON_AddObjectToObject(json, "ambience");
    cJSON_AddBoolToObject(amb, "playing", focus_sound_playing(&kind));
    cJSON_AddNumberToObject(amb, "sound", kind);
    cJSON_AddNumberToObject(amb, "cpu", focus_sound_cpu_pct());
    cJSON_AddItemToObject(json, "radio", radio_json());
    cJSON_AddItemToObject(json, "update", update_json());
    github_release_status_t release;
    github_release_get_status(&release);
    cJSON *r = cJSON_AddObjectToObject(json, "release");
    cJSON_AddBoolToObject(r, "checking", release.checking);
    cJSON_AddBoolToObject(r, "checked", release.checked);
    cJSON_AddNumberToObject(r, "lastCheck", (double)release.last_check);
    cJSON_AddNumberToObject(r, "nextCheck", (double)release.next_check);
    cJSON_AddBoolToObject(r, "s3", release.s3_available);
    cJSON_AddBoolToObject(r, "bluetooth", release.bluetooth_available);
    cJSON_AddStringToObject(r, "tag", release.tag);
    cJSON_AddStringToObject(r, "s3Version", release.s3_version);
    cJSON_AddStringToObject(r, "bluetoothVersion", release.bluetooth_version);
    cJSON_AddStringToObject(r, "error", release.error);
    // null while Bluetooth is off or the ESP32 isn't answering.
    pairing_status_t pairing;
    if (s.bluetooth && uart_link_get_pairing(&pairing)) {
        cJSON *bt = cJSON_AddObjectToObject(json, "bt");
        cJSON_AddNumberToObject(bt, "bonded", pairing.bonded);
        cJSON_AddBoolToObject(bt, "pairing", pairing.state != PAIRING_IDLE);
    } else {
        cJSON_AddNullToObject(json, "bt");
    }
    return web_send_json(req, json);
}

// ~400 names: streamed rather than built as one cJSON tree.
esp_err_t web_api_zones(httpd_req_t *req) {
    char buf[ZONES_CHUNK];
    size_t len = 0;
    httpd_resp_set_type(req, "application/json");
    // Only changes with a firmware update.
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    buf[len++] = '[';
    for (size_t i = 0; i < tz_db_count; i++) {
        // tz_db names are plain ASCII: no JSON escaping needed.
        if (len + strlen(tz_db[i].name) + 4 > sizeof(buf)) {
            if (httpd_resp_send_chunk(req, buf, len) != ESP_OK) {
                return ESP_FAIL;
            }
            len = 0;
        }
        len += snprintf(buf + len, sizeof(buf) - len, "%s\"%s\"", i ? "," : "", tz_db[i].name);
    }
    buf[len++] = ']';
    httpd_resp_send_chunk(req, buf, len);
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t web_api_set(httpd_req_t *req) {
    static const struct {
        const char *key;
        ui_setting_t setting;
        int min, max;
    } k_keys[] = {
        {"brightness", UI_SETTING_BRIGHTNESS, 5, 100},
        {"sleep", UI_SETTING_SLEEP, 0, UI_SLEEP_MAX_S},
        {"clock24", UI_SETTING_CLOCK_24H, 0, 1},
        {"fahrenheit", UI_SETTING_FAHRENHEIT, 0, 1},
        {"haptics", UI_SETTING_HAPTICS, 0, 1},
        {"artColors", UI_SETTING_ART_COLORS, 0, 1},
        {"airplayLossless", UI_SETTING_AIRPLAY_LOSSLESS, 0, 1},
        {"timerSound", UI_SETTING_TIMER_SOUND, 0, UI_TIMER_SOUND_COUNT - 1},
        {"timerVolume", UI_SETTING_TIMER_VOLUME, UI_TIMER_VOLUME_MIN, 100},
        {"bluetooth", UI_SETTING_BLUETOOTH, 0, 1},
        {"airShow", UI_SETTING_AIR_SHOW, UI_AIR_SHOW_OFF, UI_AIR_SHOW_ALWAYS},
        {"ambienceSound", UI_SETTING_AMBIENCE_SOUND, 0, UI_AMBIENCE_COUNT - 1},
        {"ambienceVolume", UI_SETTING_AMBIENCE_VOLUME, 0, 100},
        {"focusSound", UI_SETTING_FOCUS_SOUND, 0, UI_AMBIENCE_COUNT},
    };
    char form[WEB_FORM_MAX + 1], key[16], value[48];
    if (!web_read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    web_form_value(form, "key", key, sizeof(key));
    web_form_value(form, "value", value, sizeof(value));

    if (!strcmp(key, "tz")) {
        // "" is automatic; anything else has to be a zone tz_db knows.
        if (value[0] && tz_db_find(value) < 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown timezone");
        }
        settings_ui_change(UI_SETTING_TIMEZONE, 0, value[0] ? value : NULL);
        return web_send_ok(req);
    }
    for (size_t i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]); i++) {
        if (strcmp(key, k_keys[i].key)) {
            continue;
        }
        int v;
        if (!web_parse_int(value, k_keys[i].min, k_keys[i].max, &v)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Value out of range");
        }
        settings_ui_change(k_keys[i].setting, v, NULL);
        return web_send_ok(req);
    }
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown setting");
}

// A place the page found with Open-Meteo's geocoder (city, lat, lon, tz),
// auto=1 to reuse the saved detected location, or detect=1 to refresh it.
esp_err_t web_api_location(httpd_req_t *req) {
    char form[WEB_FORM_MAX + 1], text[96];
    if (!web_read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    web_form_value(form, "detect", text, sizeof(text));
    if (!strcmp(text, "1")) {
        settings_ui_change(UI_SETTING_DETECT_LOCATION, 0, NULL);
        return web_send_ok(req);
    }
    web_form_value(form, "auto", text, sizeof(text));
    if (!strcmp(text, "1")) {
        settings_ui_set_location(NULL);
        return web_send_ok(req);
    }
    geo_t geo = {0};
    char lat[24], lon[24];
    web_form_value(form, "lat", lat, sizeof(lat));
    web_form_value(form, "lon", lon, sizeof(lon));
    web_form_value(form, "city", text, sizeof(text));
    if (!text[0] || !web_parse_double(lat, -90, 90, &geo.lat) || !web_parse_double(lon, -180, 180, &geo.lon)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad location");
    }
    web_copy_utf8(geo.city, sizeof(geo.city), text);
    web_form_value(form, "tz", geo.tz, sizeof(geo.tz));
    settings_ui_set_location(&geo);
    return web_send_ok(req);
}

esp_err_t web_api_name(httpd_req_t *req) {
    char form[WEB_FORM_MAX + 1];
    if (!web_read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    char name[DEVICE_NAME_MAX * 3 + 1];
    web_form_value(form, "name", name, sizeof(name));
    if (!device_name_set(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Names need 1 to 32 characters");
    }
    return web_send_ok(req);
}

esp_err_t web_api_forget(httpd_req_t *req) {
    char form[WEB_FORM_MAX + 1];
    if (!web_read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    web_send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(FORGET_DELAY_MS));
    settings_ui_change(UI_SETTING_FORGET_WIFI, 0, NULL);
    return ESP_OK;
}
