#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airplay/receiver/bplist.h"
#include "airplay/receiver/conn.h"
#include "esp_err.h"

#define RTSP_MAX_HEADERS 32

typedef struct {
    char method[32];
    char path[128];
    int nheaders;
    char *names[RTSP_MAX_HEADERS];
    char *values[RTSP_MAX_HEADERS];
    char *head;  // backing store for the strings above
    uint8_t *body;
    size_t body_len;
} rtsp_req_t;

typedef struct {
    int code;
    char headers[768];
    size_t headers_len;
    uint8_t *body;  // freed after sending
    size_t body_len;
} rtsp_resp_t;

esp_err_t rtsp_start(void);
// Closes every sender connection.
void rtsp_disconnect_all(void);

const char *rtsp_header(const rtsp_req_t *req, const char *name);
void rtsp_resp_header(rtsp_resp_t *resp, const char *name, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
// Serializes the plist as the body; 500 if that fails.
void rtsp_resp_plist(rtsp_resp_t *resp, bpw_t *w, bpw_obj root);
bool rtsp_req_plist(const rtsp_req_t *req, bplist_t *pl);

// The /info dictionary, also sent as the event channel's updateInfo.
bpw_obj rtsp_info_plist(bpw_t *w, const ap2_conn_t *conn);
