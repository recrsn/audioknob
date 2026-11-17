#include "web/api/api.h"

#include "cJSON.h"
#include "lwip/inet.h"
#include "talk/talk.h"
#include "web/page.h"

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
