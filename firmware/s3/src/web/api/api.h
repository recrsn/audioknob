#pragma once

// The device page's JSON API, one file per area; web/server.c routes to
// these and holds the helpers they share.

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "esp_http_server.h"

#define WEB_FORM_MAX 256

// Sends `json` (freed either way) uncached; a 500 if it's NULL.
esp_err_t web_send_json(httpd_req_t *req, cJSON *json);
esp_err_t web_send_ok(httpd_req_t *req);
// Any web page the user visits could otherwise post here from their
// browser.
bool web_same_origin(httpd_req_t *req);
// Reads a POSTed form after the origin check; false means a reply has
// already been sent.
bool web_read_post(httpd_req_t *req, char *form, size_t len);
bool web_parse_int(const char *text, int min, int max, int *out);
bool web_parse_double(const char *text, double min, double max, double *out);
// Cuts a UTF-8 string to fit `len` bytes without splitting a character.
void web_copy_utf8(char *out, size_t len, const char *in);

// settings.c: every setting, the name, the location, the time zones.
esp_err_t web_api_state(httpd_req_t *req);
esp_err_t web_api_zones(httpd_req_t *req);
esp_err_t web_api_set(httpd_req_t *req);
esp_err_t web_api_location(httpd_req_t *req);
esp_err_t web_api_name(httpd_req_t *req);
esp_err_t web_api_forget(httpd_req_t *req);

// playback.c: cover art, volume, transport, ambience, radio stations.
esp_err_t web_api_art(httpd_req_t *req);
esp_err_t web_api_volume(httpd_req_t *req);
esp_err_t web_api_transport(httpd_req_t *req);
esp_err_t web_api_ambience(httpd_req_t *req);
esp_err_t web_api_stations_get(httpd_req_t *req);
esp_err_t web_api_stations_post(httpd_req_t *req);

// talk.c: LAN Talk, messages and voice notes.
esp_err_t web_api_voice_get(httpd_req_t *req);
esp_err_t web_api_voice_post(httpd_req_t *req);
esp_err_t web_api_messages_get(httpd_req_t *req);
esp_err_t web_api_messages_post(httpd_req_t *req);
esp_err_t web_api_talk_get(httpd_req_t *req);

// system.c: firmware updates, Bluetooth pairing, the mic recording.
esp_err_t web_api_update(httpd_req_t *req);
esp_err_t web_api_release(httpd_req_t *req);
esp_err_t web_api_pairing(httpd_req_t *req);
esp_err_t web_api_mic_wav(httpd_req_t *req);
