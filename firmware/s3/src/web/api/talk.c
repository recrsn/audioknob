#include "web/api/api.h"

#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "lwip/inet.h"
#include "talk/flash_message.h"
#include "talk/message_store.h"
#include "talk/talk.h"
#include "talk/voice/notes.h"
#include "web/page.h"

esp_err_t web_api_voice_get(httpd_req_t *req) {
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
    return web_send_json(req, json);
}
esp_err_t web_api_voice_post(httpd_req_t *req) {
    char form[128], action[12], id[18], peer[16];
    if (!web_read_post(req, form, sizeof(form)))
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
        return web_send_ok(req);
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
    return web_send_json(req, json);
}

esp_err_t web_api_messages_get(httpd_req_t *req) {
    return web_send_json(req, flash_message_json());
}
esp_err_t web_api_messages_post(httpd_req_t *req) {
    char form[1024], action[12], peer[16], text[UI_MESSAGE_TEXT_MAX + 2], id[18];
    if (!web_read_post(req, form, sizeof(form)))
        return ESP_OK;
    web_form_value(form, "action", action, sizeof(action));
    web_form_value(form, "id", id, sizeof(id));
    if (!strcmp(action, "ack"))
        return flash_message_ack(id) ? web_send_ok(req)
                                     : httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Cannot acknowledge message");
    if (!strcmp(action, "delete"))
        return flash_message_delete(id) ? web_send_ok(req)
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
    return web_send_json(req, json);
}
esp_err_t web_api_talk_get(httpd_req_t *req) {
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
    return web_send_json(req, json);
}
