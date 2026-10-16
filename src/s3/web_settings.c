#include "web_settings.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "artwork.h"
#include "cJSON.h"
#include "device_name.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "now_playing.h"
#include "ota.h"
#include "settings_ui.h"
#include "tz.h"
#include "uart_link.h"
#include "ui.h"
#include "web_page.h"

#define FORM_MAX     256
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
    cJSON_AddStringToObject(np, "source", state.source == SOURCE_A2DP ? "bluetooth" : "airplay");
    cJSON_AddBoolToObject(np, "connected", known && state.connected);
    cJSON_AddBoolToObject(np, "playing", known && state.playing);
    cJSON_AddNumberToObject(np, "volume", state.volume);
    cJSON_AddBoolToObject(np, "muted", state.muted);
    cJSON_AddStringToObject(np, "title", track.title);
    cJSON_AddStringToObject(np, "artist", track.artist);
    cJSON_AddStringToObject(np, "album", track.album);
    cJSON_AddNumberToObject(np, "art", art_version);
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
    cJSON_AddNumberToObject(json, "brightness", s.brightness);
    cJSON_AddNumberToObject(json, "sleep", s.sleep_s);
    cJSON_AddBoolToObject(json, "clock24", s.clock_24h);
    cJSON_AddBoolToObject(json, "fahrenheit", s.fahrenheit);
    cJSON_AddBoolToObject(json, "haptics", s.haptics);
    cJSON_AddBoolToObject(json, "bluetooth", s.bluetooth);
    cJSON_AddNumberToObject(json, "airplay", s.airplay);
    cJSON_AddBoolToObject(json, "airplay2", s.airplay2_available);
    cJSON_AddBoolToObject(json, "tzAuto", s.tz_auto);
    cJSON_AddStringToObject(json, "tz", s.tz_name);
    cJSON_AddBoolToObject(json, "locationAuto", s.location_auto);
    cJSON_AddStringToObject(json, "location", s.location);
    cJSON_AddItemToObject(json, "np", now_playing_json());
    cJSON_AddItemToObject(json, "update", update_json());
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
        {"bluetooth", UI_SETTING_BLUETOOTH, 0, 1},
        {"airplay", UI_SETTING_AIRPLAY, 0, UI_AIRPLAY_1 | UI_AIRPLAY_2},
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
// or auto=1 to go back to the IP-detected one.
static esp_err_t location_handler(httpd_req_t *req) {
    char form[FORM_MAX + 1], text[96];
    if (!read_post(req, form, sizeof(form))) {
        return ESP_OK;
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
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ota.error);
    // Closes the socket instead of draining what's left of a refused
    // image; the browser may see that as a network error, so the page
    // reads the reason from /api/state.
    return ESP_FAIL;
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
        {.uri = "/api/set", .method = HTTP_POST, .handler = set_handler},
        {.uri = "/api/name", .method = HTTP_POST, .handler = name_handler},
        {.uri = "/api/location", .method = HTTP_POST, .handler = location_handler},
        {.uri = "/api/volume", .method = HTTP_POST, .handler = volume_handler},
        {.uri = "/api/transport", .method = HTTP_POST, .handler = transport_handler},
        {.uri = "/api/pairing", .method = HTTP_POST, .handler = pairing_handler},
        {.uri = "/api/forget", .method = HTTP_POST, .handler = forget_handler},
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
