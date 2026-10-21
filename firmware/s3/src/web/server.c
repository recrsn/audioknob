#include "web/server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "web/api/api.h"
#include "web/page.h"

#define HEADER_MAX 96

static const char *TAG = "web_settings";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static httpd_handle_t s_server;

esp_err_t web_send_json(httpd_req_t *req, cJSON *json) {
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

esp_err_t web_send_ok(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    // EMBED_TXTFILES appends a NUL.
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

// Any web page the user visits could otherwise post here from their
// browser. Browsers send Origin on cross-site POSTs; same-origin ones
// match Host.
bool web_same_origin(httpd_req_t *req) {
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
bool web_read_post(httpd_req_t *req, char *form, size_t len) {
    if (!web_same_origin(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-site request refused");
        return false;
    }
    if (!web_read_form(req, form, len)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad form");
        return false;
    }
    return true;
}

bool web_parse_int(const char *text, int min, int max, int *out) {
    char *end;
    long v = strtol(text, &end, 10);
    if (!*text || *end || v < min || v > max) {
        return false;
    }
    *out = (int)v;
    return true;
}

bool web_parse_double(const char *text, double min, double max, double *out) {
    char *end;
    double v = strtod(text, &end);
    if (!*text || *end || !(v >= min && v <= max)) {
        return false;
    }
    *out = v;
    return true;
}

// Cuts a UTF-8 string to fit `len` bytes without splitting a character.
void web_copy_utf8(char *out, size_t len, const char *in) {
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

bool web_settings_start(void) {
    if (s_server) {
        return true;
    }
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/api/state", .method = HTTP_GET, .handler = web_api_state},
        {.uri = "/api/zones", .method = HTTP_GET, .handler = web_api_zones},
        {.uri = "/api/art.bmp", .method = HTTP_GET, .handler = web_api_art},
        {.uri = "/api/voice", .method = HTTP_GET, .handler = web_api_voice_get},
        {.uri = "/api/voice", .method = HTTP_POST, .handler = web_api_voice_post},
        {.uri = "/api/talk", .method = HTTP_GET, .handler = web_api_talk_get},
        {.uri = "/api/messages", .method = HTTP_GET, .handler = web_api_messages_get},
        {.uri = "/api/messages", .method = HTTP_POST, .handler = web_api_messages_post},
        {.uri = "/api/mic.wav", .method = HTTP_GET, .handler = web_api_mic_wav},
        {.uri = "/api/set", .method = HTTP_POST, .handler = web_api_set},
        {.uri = "/api/name", .method = HTTP_POST, .handler = web_api_name},
        {.uri = "/api/location", .method = HTTP_POST, .handler = web_api_location},
        {.uri = "/api/volume", .method = HTTP_POST, .handler = web_api_volume},
        {.uri = "/api/transport", .method = HTTP_POST, .handler = web_api_transport},
        {.uri = "/api/ambience", .method = HTTP_POST, .handler = web_api_ambience},
        {.uri = "/api/pairing", .method = HTTP_POST, .handler = web_api_pairing},
        {.uri = "/api/stations", .method = HTTP_GET, .handler = web_api_stations_get},
        {.uri = "/api/stations", .method = HTTP_POST, .handler = web_api_stations_post},
        {.uri = "/api/forget", .method = HTTP_POST, .handler = web_api_forget},
        {.uri = "/api/releases", .method = HTTP_POST, .handler = web_api_release},
        {.uri = "/api/update", .method = HTTP_POST, .handler = web_api_update},
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
