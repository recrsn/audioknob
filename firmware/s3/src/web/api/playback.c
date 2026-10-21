#include "web/api/api.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "airplay/artwork/artwork.h"
#include "ambience/focus_sound.h"
#include "cJSON.h"
#include "core/now_playing.h"
#include "core/settings/ui.h"
#include "esp_heap_caps.h"
#include "radio/radio.h"
#include "radio/stations.h"
#include "web/page.h"

// A station: a URL of up to 255 bytes and a name, both percent-encoded.
#define STATION_FORM_MAX 1100

// A 16-bit BI_BITFIELDS BMP straight from the RGB565 bitmap: no encoder
// needed, and every browser shows it.
esp_err_t web_api_art(httpd_req_t *req) {
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

esp_err_t web_api_volume(httpd_req_t *req) {
    char form[WEB_FORM_MAX + 1], text[8];
    if (!web_read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    int v;
    bool used;
    web_form_value(form, "value", text, sizeof(text));
    if (web_parse_int(text, 0, 100, &v)) {
        used = now_playing_set_volume((uint8_t)v);
    } else {
        web_form_value(form, "step", text, sizeof(text));
        if (!web_parse_int(text, -100, 100, &v)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad volume");
        }
        used = now_playing_adjust_volume(v);
    }
    return used ? web_send_ok(req) : httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Nothing is playing");
}

esp_err_t web_api_transport(httpd_req_t *req) {
    char form[WEB_FORM_MAX + 1], action[8];
    if (!web_read_post(req, form, sizeof(form))) {
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
    return web_send_ok(req);
}

// Starts the default sound (or `sound`), or stops. The device screen
// follows on its next poll.
esp_err_t web_api_ambience(httpd_req_t *req) {
    char form[WEB_FORM_MAX + 1], action[8], sound[4];
    if (!web_read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    web_form_value(form, "action", action, sizeof(action));
    if (!strcmp(action, "stop")) {
        focus_sound_stop();
        return web_send_ok(req);
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
    } else if (!web_parse_int(sound, 0, AMBIENCE_COUNT - 1, &v)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown sound");
    }
    focus_sound_play((ambience_kind_t)v);
    return web_send_ok(req);
}

esp_err_t web_api_stations_get(httpd_req_t *req) {
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
    return web_send_json(req, list);
}

// The station presets: add (name, url), update (index, name, url), remove
// (index), move (index, to), play (index) or stop. Searching for stations is
// the page's job, done from the phone against radio-browser.info.
esp_err_t web_api_stations_post(httpd_req_t *req) {
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
    if (!web_read_post(req, b->form, sizeof(b->form))) {
        goto done;
    }
    web_form_value(b->form, "action", b->action, sizeof(b->action));
    web_form_value(b->form, "index", b->index, sizeof(b->index));
    web_form_value(b->form, "to", b->to, sizeof(b->to));
    int index = 0, to = 0;
    bool has_index = web_parse_int(b->index, 0, RADIO_STATIONS_MAX - 1, &index);
    bool ok;
    const char *why = "Unknown station";
    if (!strcmp(b->action, "add")) {
        web_form_value(b->form, "name", b->name, sizeof(b->name));
        web_form_value(b->form, "url", b->url, sizeof(b->url));
        char name[RADIO_NAME_MAX];
        web_copy_utf8(name, sizeof(name), b->name);
        ok = radio_stations_add(name, b->url);
        why = radio_stations_count() >= RADIO_STATIONS_MAX ? "The list is full"
                                                           : "Needs a name and an http(s):// address";
    } else if (!strcmp(b->action, "update")) {
        web_form_value(b->form, "name", b->name, sizeof(b->name));
        web_form_value(b->form, "url", b->url, sizeof(b->url));
        char name[RADIO_NAME_MAX];
        web_copy_utf8(name, sizeof(name), b->name);
        ok = has_index && radio_stations_update(index, name, b->url);
        why = "Needs a name and an http(s):// address";
    } else if (!strcmp(b->action, "remove")) {
        ok = has_index && radio_stations_remove(index);
    } else if (!strcmp(b->action, "move")) {
        ok = has_index && web_parse_int(b->to, 0, RADIO_STATIONS_MAX - 1, &to) && radio_stations_move(index, to);
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
    err = ok ? web_send_ok(req) : httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
done:
    heap_caps_free(b);
    return err;
}
