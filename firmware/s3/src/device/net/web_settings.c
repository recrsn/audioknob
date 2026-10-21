#include "web_settings.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airplay.h"
#include "artwork.h"
#include "cJSON.h"
#include "device_name.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "focus_sound.h"
#include "github_release.h"
#include "mic.h"
#include "talk.h"
#include "flash_message.h"
#include "voice_notes.h"
#include "message_store.h"
#include "lwip/inet.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "now_playing.h"
#include "online.h"
#include "ota.h"
#include "radio.h"
#include "radio_stations.h"
#include "settings_ui.h"
#include "tz.h"
#include "uart_link.h"
#include "ui.h"
#include "web_page.h"

#define FORM_MAX     256
// A station: a URL of up to 255 bytes and a name, both percent-encoded.
#define STATION_FORM_MAX 1100
#define HEADER_MAX   96
#define ZONES_CHUNK  1024
// Lets the reply reach the browser before the device restarts.
#define FORGET_DELAY_MS 300
// httpd_req_recv() gives up after the server's 5 s receive timeout; a
// phone that pauses mid-upload gets a few of those.
#define UPLOAD_RETRIES 3

static const char *TAG = "web_settings";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;

static esp_err_t send_json(httpd_req_t *req, cJSON *json) {
    char *text = json ? cJSON_PrintUnformatted(json) : NULL;
    cJSON_Delete(json);
    if (!text) {
        return httpd_resp_send_500(req);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}

static esp_err_t send_ok(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    // EMBED_TXTFILES appends a NUL.
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

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

static esp_err_t state_handler(httpd_req_t *req) {
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
    return send_json(req, json);
}

// ~400 names: streamed rather than built as one cJSON tree.
static esp_err_t zones_handler(httpd_req_t *req) {
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

// A 16-bit BI_BITFIELDS BMP straight from the RGB565 bitmap: no encoder
// needed, and every browser shows it.
static esp_err_t art_handler(httpd_req_t *req) {
    enum { ROW = ARTWORK_SIZE * 2, PIXELS = ROW * ARTWORK_SIZE, HEADER = 14 + 40 + 12 };
    uint32_t version;
    const uint16_t *art = now_playing_artwork(&version);
    if (!art) {
        return httpd_resp_send_404(req);
    }
    // A copy, so a new decode can't change the image mid-send.
    uint8_t *bmp = heap_caps_malloc(HEADER + PIXELS, MALLOC_CAP_SPIRAM);
    if (!bmp) {
        return httpd_resp_send_500(req);
    }
    memcpy(bmp + HEADER, art, PIXELS);
    static_assert(ROW % 4 == 0, "BMP rows must be 4-byte aligned");
    const uint32_t fields[] = {
        // BITMAPFILEHEADER (the 'BM' tag is written below)
        HEADER + PIXELS, 0, HEADER,
        // BITMAPINFOHEADER; negative height = top-down rows
        40, ARTWORK_SIZE, (uint32_t)-ARTWORK_SIZE, 16 << 16 | 1, 3, PIXELS, 2835, 2835, 0, 0,
        // R, G, B masks
        0xF800, 0x07E0, 0x001F,
    };
    bmp[0] = 'B';
    bmp[1] = 'M';
    memcpy(bmp + 2, fields, sizeof(fields));
    httpd_resp_set_type(req, "image/bmp");
    // The page asks for it by version.
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    esp_err_t err = httpd_resp_send(req, (const char *)bmp, HEADER + PIXELS);
    free(bmp);
    return err;
}

// Any web page the user visits could otherwise post here from their
// browser. Browsers send Origin on cross-site POSTs; same-origin ones
// match Host.
static bool same_origin(httpd_req_t *req) {
    char origin[HEADER_MAX], host[HEADER_MAX], expected[HEADER_MAX + 8];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) {
        return true;
    }
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    snprintf(expected, sizeof(expected), "http://%s", host);
    return !strcmp(origin, expected);
}

// Reads a POSTed form after the origin check; false means a reply has
// already been sent.
static bool read_post(httpd_req_t *req, char *form, size_t len) {
    if (!same_origin(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-site request refused");
        return false;
    }
    if (!web_read_form(req, form, len)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad form");
        return false;
    }
    return true;
}

static bool parse_int(const char *text, int min, int max, int *out) {
    char *end;
    long v = strtol(text, &end, 10);
    if (!*text || *end || v < min || v > max) {
        return false;
    }
    *out = (int)v;
    return true;
}

// Debug: records ?s= seconds (1-30) from the mic as a 16 kHz mono WAV.
// ?raw=1 bypasses processing; ?gain=0..36 and ?ns=0 tune voice processing.
// ?slot=r reads the other PDM clock edge.
static esp_err_t mic_wav_handler(httpd_req_t *req) {
    enum { HEADER = 44, MAX_SECONDS = 30 };
    char query[96] = "", value[8];
    httpd_req_get_url_query_str(req, query, sizeof(query));
    int seconds = 5;
    if (httpd_query_key_value(query, "s", value, sizeof(value)) == ESP_OK && !parse_int(value, 1, MAX_SECONDS, &seconds)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "s must be 1-30");
    }
    bool raw = httpd_query_key_value(query, "raw", value, sizeof(value)) == ESP_OK && !strcmp(value, "1");
    bool right = httpd_query_key_value(query, "slot", value, sizeof(value)) == ESP_OK && !strcmp(value, "r");

    int gain = MIC_DEFAULT_GAIN_DB;
    if (httpd_query_key_value(query, "gain", value, sizeof(value)) == ESP_OK &&
        !parse_int(value, 0, MIC_MAX_GAIN_DB, &gain)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gain must be 0-36 dB");
    }
    int noise_level = MIC_DEFAULT_NOISE_LEVEL;
    if (httpd_query_key_value(query, "ns", value, sizeof(value)) == ESP_OK) {
        if (!parse_int(value, 0, MIC_MAX_NOISE_LEVEL, &noise_level)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ns must be 0-3");
        }
    }
    size_t frames = (size_t)seconds * MIC_SAMPLE_RATE;
    uint32_t data_bytes = frames * sizeof(int16_t);
    uint8_t *wav = heap_caps_malloc(HEADER + data_bytes, MALLOC_CAP_SPIRAM);
    if (!wav) {
        return httpd_resp_send_500(req);
    }
    const mic_config_t config = {.gain_db = (uint8_t)gain, .noise_level = (uint8_t)noise_level,
                                 .raw = raw, .right_slot = right};
    if (!mic_start(&config)) {
        free(wav);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Mic busy or processing memory unavailable");
    }
    int16_t *pcm = (int16_t *)(wav + HEADER);
    size_t got = 0;
    while (got < frames) {
        size_t n = mic_read(pcm + got, frames - got, 1000);
        if (!n) {
            break;
        }
        got += n;
    }
    mic_stop();
    data_bytes = got * sizeof(int16_t);

    const uint32_t byte_rate = MIC_SAMPLE_RATE * sizeof(int16_t);
    const uint32_t fields[] = {
        36 + data_bytes, 0x45564157 /* WAVE */, 0x20746d66 /* fmt  */, 16,
        1 | 1 << 16 /* PCM, mono */, MIC_SAMPLE_RATE, byte_rate, 2 | 16 << 16 /* align, bits */,
        0x61746164 /* data */, data_bytes,
    };
    memcpy(wav, "RIFF", 4);
    memcpy(wav + 4, fields, sizeof(fields));
    char cost[16];
    snprintf(cost, sizeof(cost), "%.2f", (double)mic_processing_cpu_pct());
    httpd_resp_set_hdr(req, "X-Mic-CPU-Percent", cost);
    httpd_resp_set_type(req, "audio/wav");
    esp_err_t err = httpd_resp_send(req, (const char *)wav, HEADER + data_bytes);
    free(wav);
    return err;
}

static esp_err_t set_handler(httpd_req_t *req) {
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
    char form[FORM_MAX + 1], key[16], value[48];
    if (!read_post(req, form, sizeof(form))) {
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
        return send_ok(req);
    }
    for (size_t i = 0; i < sizeof(k_keys) / sizeof(k_keys[0]); i++) {
        if (strcmp(key, k_keys[i].key)) {
            continue;
        }
        int v;
        if (!parse_int(value, k_keys[i].min, k_keys[i].max, &v)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Value out of range");
        }
        settings_ui_change(k_keys[i].setting, v, NULL);
        return send_ok(req);
    }
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown setting");
}

static bool parse_double(const char *text, double min, double max, double *out) {
    char *end;
    double v = strtod(text, &end);
    if (!*text || *end || !(v >= min && v <= max)) {
        return false;
    }
    *out = v;
    return true;
}

// Cuts a UTF-8 string to fit `len` bytes without splitting a character.
static void copy_utf8(char *out, size_t len, const char *in) {
    size_t n = strlen(in);
    if (n >= len) {
        n = len - 1;
        while (n && ((unsigned char)in[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(out, in, n);
    out[n] = '\0';
}

// A place the page found with Open-Meteo's geocoder (city, lat, lon, tz),
// auto=1 to reuse the saved detected location, or detect=1 to refresh it.
static esp_err_t location_handler(httpd_req_t *req) {
    char form[FORM_MAX + 1], text[96];
    if (!read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    web_form_value(form, "detect", text, sizeof(text));
    if (!strcmp(text, "1")) {
        settings_ui_change(UI_SETTING_DETECT_LOCATION, 0, NULL);
        return send_ok(req);
    }
    web_form_value(form, "auto", text, sizeof(text));
    if (!strcmp(text, "1")) {
        settings_ui_set_location(NULL);
        return send_ok(req);
    }
    geo_t geo = {0};
    char lat[24], lon[24];
    web_form_value(form, "lat", lat, sizeof(lat));
    web_form_value(form, "lon", lon, sizeof(lon));
    web_form_value(form, "city", text, sizeof(text));
    if (!text[0] || !parse_double(lat, -90, 90, &geo.lat) || !parse_double(lon, -180, 180, &geo.lon)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad location");
    }
    copy_utf8(geo.city, sizeof(geo.city), text);
    web_form_value(form, "tz", geo.tz, sizeof(geo.tz));
    settings_ui_set_location(&geo);
    return send_ok(req);
}

static esp_err_t name_handler(httpd_req_t *req) {
    char form[FORM_MAX + 1];
    if (!read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    char name[DEVICE_NAME_MAX * 3 + 1];
    web_form_value(form, "name", name, sizeof(name));
    if (!device_name_set(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Names need 1 to 32 characters");
    }
    return send_ok(req);
}

static esp_err_t volume_handler(httpd_req_t *req) {
    char form[FORM_MAX + 1], text[8];
    if (!read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    int v;
    bool used;
    web_form_value(form, "value", text, sizeof(text));
    if (parse_int(text, 0, 100, &v)) {
        used = now_playing_set_volume((uint8_t)v);
    } else {
        web_form_value(form, "step", text, sizeof(text));
        if (!parse_int(text, -100, 100, &v)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad volume");
        }
        used = now_playing_adjust_volume(v);
    }
    return used ? send_ok(req) : httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Nothing is playing");
}

static esp_err_t transport_handler(httpd_req_t *req) {
    char form[FORM_MAX + 1], action[8];
    if (!read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    web_form_value(form, "action", action, sizeof(action));
    if (!strcmp(action, "prev")) {
        now_playing_transport(UI_TRANSPORT_PREV);
    } else if (!strcmp(action, "play")) {
        now_playing_transport(UI_TRANSPORT_PLAY_PAUSE);
    } else if (!strcmp(action, "next")) {
        now_playing_transport(UI_TRANSPORT_NEXT);
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
    }
    return send_ok(req);
}

// Starts the default sound (or `sound`), or stops. The device screen
// follows on its next poll.
static esp_err_t ambience_handler(httpd_req_t *req) {
    char form[FORM_MAX + 1], action[8], sound[4];
    if (!read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    web_form_value(form, "action", action, sizeof(action));
    if (!strcmp(action, "stop")) {
        focus_sound_stop();
        return send_ok(req);
    }
    if (strcmp(action, "play")) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
    }
    int v;
    web_form_value(form, "sound", sound, sizeof(sound));
    if (!sound[0]) {
        ui_settings_t s;
        settings_ui_snapshot(&s);
        v = s.ambience_sound;
    } else if (!parse_int(sound, 0, AMBIENCE_COUNT - 1, &v)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown sound");
    }
    focus_sound_play((ambience_kind_t)v);
    return send_ok(req);
}

// Opens the pairing window, or forgets every paired device. The code
// check itself stays on the device's screen.
static esp_err_t pairing_handler(httpd_req_t *req) {
    char form[FORM_MAX + 1], action[8];
    if (!read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    pairing_status_t pairing;
    if (!uart_link_get_pairing(&pairing)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bluetooth isn't on");
    }
    web_form_value(form, "action", action, sizeof(action));
    if (!strcmp(action, "open")) {
        uart_link_send_command(CMD_PAIRING_OPEN);
    } else if (!strcmp(action, "forget")) {
        uart_link_send_command(CMD_FORGET_DEVICES);
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
    }
    return send_ok(req);
}

static esp_err_t stations_get_handler(httpd_req_t *req) {
    cJSON *list = cJSON_CreateArray();
    int count = radio_stations_count();
    for (int i = 0; i < count; i++) {
        radio_station_t station;
        if (radio_stations_get(i, &station)) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "name", station.name);
            cJSON_AddStringToObject(item, "url", station.url);
            cJSON_AddItemToArray(list, item);
        }
    }
    return send_json(req, list);
}

// The station presets: add (name, url), update (index, name, url), remove
// (index), move (index, to), play (index) or stop. Searching for stations is
// the page's job, done from the phone against radio-browser.info.
static esp_err_t stations_post_handler(httpd_req_t *req) {
    enum { NAME_ENC = RADIO_NAME_MAX * 3, URL_ENC = STREAM_URL_MAX * 3 };
    typedef struct {
        char form[STATION_FORM_MAX + 1];
        char name[NAME_ENC + 1];
        char url[URL_ENC + 1];
        char action[8], index[8], to[8];
    } buf_t;
    buf_t *b = heap_caps_malloc(sizeof(*b), MALLOC_CAP_SPIRAM);
    if (!b) {
        return httpd_resp_send_500(req);
    }
    esp_err_t err = ESP_OK;
    if (!read_post(req, b->form, sizeof(b->form))) {
        goto done;
    }
    web_form_value(b->form, "action", b->action, sizeof(b->action));
    web_form_value(b->form, "index", b->index, sizeof(b->index));
    web_form_value(b->form, "to", b->to, sizeof(b->to));
    int index = 0, to = 0;
    bool has_index = parse_int(b->index, 0, RADIO_STATIONS_MAX - 1, &index);
    bool ok;
    const char *why = "Unknown station";
    if (!strcmp(b->action, "add")) {
        web_form_value(b->form, "name", b->name, sizeof(b->name));
        web_form_value(b->form, "url", b->url, sizeof(b->url));
        char name[RADIO_NAME_MAX];
        copy_utf8(name, sizeof(name), b->name);
        ok = radio_stations_add(name, b->url);
        why = radio_stations_count() >= RADIO_STATIONS_MAX ? "The list is full"
                                                           : "Needs a name and an http(s):// address";
    } else if (!strcmp(b->action, "update")) {
        web_form_value(b->form, "name", b->name, sizeof(b->name));
        web_form_value(b->form, "url", b->url, sizeof(b->url));
        char name[RADIO_NAME_MAX];
        copy_utf8(name, sizeof(name), b->name);
        ok = has_index && radio_stations_update(index, name, b->url);
        why = "Needs a name and an http(s):// address";
    } else if (!strcmp(b->action, "remove")) {
        ok = has_index && radio_stations_remove(index);
    } else if (!strcmp(b->action, "move")) {
        ok = has_index && parse_int(b->to, 0, RADIO_STATIONS_MAX - 1, &to) && radio_stations_move(index, to);
    } else if (!strcmp(b->action, "play")) {
        ok = has_index && index < radio_stations_count();
        if (ok) {
            radio_play(index);
        }
    } else if (!strcmp(b->action, "stop")) {
        radio_stop();
        ok = true;
    } else {
        ok = false;
        why = "Unknown action";
    }
    err = ok ? send_ok(req) : httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
done:
    heap_caps_free(b);
    return err;
}

static esp_err_t forget_handler(httpd_req_t *req) {
    char form[FORM_MAX + 1];
    if (!read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    send_ok(req);
    vTaskDelay(pdMS_TO_TICKS(FORGET_DELAY_MS));
    settings_ui_change(UI_SETTING_FORGET_WIFI, 0, NULL);
    return ESP_OK;
}

static int read_upload(void *ctx, char *buf, size_t len) {
    for (int tries = 0; tries < UPLOAD_RETRIES; tries++) {
        int n = httpd_req_recv(ctx, buf, len);
        if (n != HTTPD_SOCK_ERR_TIMEOUT) {
            return n;
        }
    }
    return -1;
}

// The raw image as the request body. It only lands in the spare slot: the
// device asks before booting it.
static esp_err_t update_handler(httpd_req_t *req) {
    if (!same_origin(req)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-site request refused");
    }
    if (ota_receive(req->content_len, read_upload, req)) {
        return send_ok(req);
    }
    ota_status_t ota;
    ota_get_status(&ota);
    // No error of its own when an install is running: that one's status stays.
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ota.error[0] ? ota.error : "Another update is pending or in progress");
    // Closes the socket instead of draining what's left of a refused
    // image; the browser may see that as a network error, so the page
    // reads the reason from /api/state.
    return ESP_FAIL;
}

static esp_err_t release_handler(httpd_req_t *req) {
    char form[FORM_MAX + 1], action[16], target[16];
    if (!read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    web_form_value(form, "action", action, sizeof(action));
    web_form_value(form, "target", target, sizeof(target));
    bool ok;
    if (!strcmp(action, "check")) {
        ok = github_release_check();
    } else if (!strcmp(action, "download") && (!strcmp(target, "s3") || !strcmp(target, "bluetooth"))) {
        ok = github_release_download(!strcmp(target, "s3") ? OTA_TARGET_S3 : OTA_TARGET_ESP32);
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown release action or target");
    }
    return ok ? send_ok(req) : httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
        "Update busy or unavailable; check for updates first");
}

static esp_err_t voice_get_handler(httpd_req_t *req) {
    char query[64] = "", id[18] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
        web_form_value(query, "id", id, sizeof(id));
    if (id[0]) {
        size_t length;
        void *data = voice_notes_read(id, &length);
        if (!data)
            return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Voice note not found");
        httpd_resp_set_type(req, "audio/ogg");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        esp_err_t err = httpd_resp_send(req, data, length);
        free(data);
        return err;
    }
    ui_voice_t voice;
    voice_notes_get_state(&voice);
    size_t total, used;
    message_store_usage(&total, &used);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "state", voice.state);
    cJSON_AddStringToObject(json, "id", voice.id);
    cJSON_AddStringToObject(json, "message", voice.message);
    cJSON_AddNumberToObject(json, "seconds", voice.seconds);
    cJSON_AddBoolToObject(json, "storageReady", voice.storage_ready);
    cJSON_AddNumberToObject(json, "storageBytes", total);
    cJSON_AddNumberToObject(json, "usedBytes", used);
    return send_json(req, json);
}
static esp_err_t voice_post_handler(httpd_req_t *req) {
    char form[128], action[12], id[18], peer[16];
    if (!read_post(req, form, sizeof(form)))
        return ESP_OK;
    web_form_value(form, "action", action, sizeof(action));
    web_form_value(form, "id", id, sizeof(id));
    web_form_value(form, "peer", peer, sizeof(peer));
    struct in_addr addr = {0};
    if (peer[0] && (!inet_aton(peer, &addr) || !addr.s_addr))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad peer address");
    bool ok = false;
    if (!strcmp(action, "stop")) {
        voice_notes_stop();
        return send_ok(req);
    }
    if (!strcmp(action, "record"))
        ok = voice_notes_record(addr.s_addr, id);
    else if (!strcmp(action, "play"))
        ok = voice_notes_play(id);
    else if (!strcmp(action, "send"))
        ok = flash_message_send_voice(addr.s_addr, id);
    if (!ok)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Voice note unavailable, busy, or storage full");
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "id", id);
    cJSON_AddStringToObject(json, "status", "queued");
    httpd_resp_set_status(req, "202 Accepted");
    return send_json(req, json);
}

static esp_err_t messages_get_handler(httpd_req_t *req) {
    return send_json(req, flash_message_json());
}
static esp_err_t messages_post_handler(httpd_req_t *req) {
    char form[1024], action[12], peer[16], text[UI_MESSAGE_TEXT_MAX + 2], id[18];
    if (!read_post(req, form, sizeof(form)))
        return ESP_OK;
    web_form_value(form, "action", action, sizeof(action));
    web_form_value(form, "id", id, sizeof(id));
    if (!strcmp(action, "ack"))
        return flash_message_ack(id) ? send_ok(req)
                                     : httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Cannot acknowledge message");
    if (!strcmp(action, "delete"))
        return flash_message_delete(id) ? send_ok(req)
                                        : httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Cannot delete message");
    web_form_value(form, "peer", peer, sizeof(peer));
    web_form_value(form, "text", text, sizeof(text));
    struct in_addr addr;
    if (strcmp(action, "send") || inet_aton(peer, &addr) == 0 || !addr.s_addr ||
        !flash_message_send(addr.s_addr, text, id))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad message or outbox full");
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "id", id);
    cJSON_AddStringToObject(json, "status", "queued");
    httpd_resp_set_status(req, "202 Accepted");
    return send_json(req, json);
}
static esp_err_t talk_get_handler(httpd_req_t *req) {
    ui_talk_t state;
    talk_get_state(&state);
    cJSON *json = cJSON_CreateObject(), *peers = cJSON_AddArrayToObject(json, "peers");
    for (int i = 0; i < state.count; i++) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddItemToArray(peers, p);
        struct in_addr addr = {.s_addr = state.peers[i].id};
        cJSON_AddStringToObject(p, "ip", inet_ntoa(addr));
        cJSON_AddStringToObject(p, "name", state.peers[i].name);
    }
    cJSON_AddNumberToObject(json, "state", state.state);
    cJSON_AddNumberToObject(json, "volume", state.volume);
    cJSON_AddStringToObject(json, "peer", state.peer);
    return send_json(req, json);
}
bool web_settings_start(void) {
    if (s_server) {
        return true;
    }
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/api/state", .method = HTTP_GET, .handler = state_handler},
        {.uri = "/api/zones", .method = HTTP_GET, .handler = zones_handler},
        {.uri = "/api/art.bmp", .method = HTTP_GET, .handler = art_handler},
        {.uri = "/api/voice", .method = HTTP_GET, .handler = voice_get_handler},
        {.uri = "/api/voice", .method = HTTP_POST, .handler = voice_post_handler},
        {.uri = "/api/talk", .method = HTTP_GET, .handler = talk_get_handler},
        {.uri = "/api/messages", .method = HTTP_GET, .handler = messages_get_handler},
        {.uri = "/api/messages", .method = HTTP_POST, .handler = messages_post_handler},
        {.uri = "/api/mic.wav", .method = HTTP_GET, .handler = mic_wav_handler},
        {.uri = "/api/set", .method = HTTP_POST, .handler = set_handler},
        {.uri = "/api/name", .method = HTTP_POST, .handler = name_handler},
        {.uri = "/api/location", .method = HTTP_POST, .handler = location_handler},
        {.uri = "/api/volume", .method = HTTP_POST, .handler = volume_handler},
        {.uri = "/api/transport", .method = HTTP_POST, .handler = transport_handler},
        {.uri = "/api/ambience", .method = HTTP_POST, .handler = ambience_handler},
        {.uri = "/api/pairing", .method = HTTP_POST, .handler = pairing_handler},
        {.uri = "/api/stations", .method = HTTP_GET, .handler = stations_get_handler},
        {.uri = "/api/stations", .method = HTTP_POST, .handler = stations_post_handler},
        {.uri = "/api/forget", .method = HTTP_POST, .handler = forget_handler},
        {.uri = "/api/releases", .method = HTTP_POST, .handler = release_handler},
        {.uri = "/api/update", .method = HTTP_POST, .handler = update_handler},
    };
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    // esp_ota_end()'s image check runs on this stack too.
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = sizeof(uris) / sizeof(uris[0]);
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        s_server = NULL;
        return false;
    }
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "device page up");
    return true;
}
