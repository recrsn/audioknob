/*
 * AirPlay 2 RTSP server: connection handling, request parsing, encrypted
 * framing and the connection-level handlers (info, pairing, parameters).
 * Stream handlers live in session.c.
 *
 * Request flow, handlers and their responses follow shairport-sync's
 * rtsp.c (Copyright (c) James Laird 2013, Mike Brady 2014--2026, MIT
 * License); pairing is owntone's pair_ap (pair_ap/). AirPlay 1 senders'
 * stream methods go to ap1.c.
 */

#include "rtsp.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ap1.h"
#include "ap2.h"
#include "dmap.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "event.h"
#include "fp_setup.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "session.h"

#define MAX_CONNS 4
#define HEAD_MAX 4096
#define BODY_MAX (512 * 1024)
#define READ_CHUNK 1024
#define CONN_STACK 8192
#define CONN_PRIO 6
#define LISTEN_STACK 3072
#define LISTEN_PRIO 6
#define RECV_TIMEOUT_MS 1000

static const char *TAG = "ap2_rtsp";

static ap2_conn_t *s_conns[MAX_CONNS];
static SemaphoreHandle_t s_conns_lock;
static int s_next_id = 1;

/* Pairings registered through pair-setup (HomeKit), kept in RAM like
 * shairport-sync does; senders redo pair-setup after a reboot. */

typedef struct pairing {
    char device_id[PAIR_AP_DEVICE_ID_LEN_MAX];
    uint8_t public_key[32];
    struct pairing *next;
} pairing_t;

static pairing_t *s_pairings;
static SemaphoreHandle_t s_pairings_lock;

/* Encrypted streams */

void cipher_stream_free(cipher_stream_t *cs) {
    if (cs->ctx) {
        pair_cipher_free(cs->ctx);
    }
    free(cs->cipher);
    free(cs->plain);
    memset(cs, 0, sizeof(*cs));
}

static bool append(uint8_t **buf, size_t *len, size_t *cap, const uint8_t *data, size_t n) {
    if (*len + n > *cap) {
        size_t c = *len + n + 2048;
        uint8_t *p = realloc(*buf, c);
        if (!p) {
            return false;
        }
        *buf = p;
        *cap = c;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    return true;
}

ssize_t cipher_stream_read(int fd, cipher_stream_t *cs, void *buf, size_t len, volatile bool *stop) {
    while (cs->plain_pos == cs->plain_len) {
        uint8_t in[1024];
        int n = ap2_recv(fd, in, sizeof(in), stop);
        if (n <= 0) {
            return n;
        }
        if (!append(&cs->cipher, &cs->cipher_len, &cs->cipher_cap, in, (size_t)n)) {
            return -1;
        }
        uint8_t *plain = NULL;
        size_t plain_len = 0;
        ssize_t used = pair_decrypt(&plain, &plain_len, cs->cipher, cs->cipher_len, cs->ctx);
        if (used < 0) {
            ESP_LOGW(TAG, "decrypt: %s", pair_cipher_errmsg(cs->ctx));
            return -1;
        }
        memmove(cs->cipher, cs->cipher + used, cs->cipher_len - (size_t)used);
        cs->cipher_len -= (size_t)used;
        free(cs->plain);
        cs->plain = plain;
        cs->plain_len = plain_len;
        cs->plain_pos = 0;
    }
    size_t n = cs->plain_len - cs->plain_pos;
    n = n < len ? n : len;
    memcpy(buf, cs->plain + cs->plain_pos, n);
    cs->plain_pos += n;
    return (ssize_t)n;
}

static bool write_all(int fd, const uint8_t *buf, size_t len) {
    while (len > 0) {
        int n = send(fd, buf, len, 0);
        if (n <= 0) {
            return false;
        }
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

bool cipher_stream_write(int fd, cipher_stream_t *cs, const void *buf, size_t len) {
    uint8_t *out = NULL;
    size_t out_len = 0;
    if (pair_encrypt(&out, &out_len, buf, len, cs->ctx) < 0) {
        ESP_LOGW(TAG, "encrypt: %s", pair_cipher_errmsg(cs->ctx));
        return false;
    }
    bool ok = write_all(fd, out, out_len);
    free(out);
    return ok;
}

// Some senders pair and then carry on in clear (pyatv's AirPlay 1 mode).
// A cipher block starts with its little-endian length, at most 1024, so a
// second byte above 4 is no block; an upper-case first byte is a method.
static void check_cipher(ap2_conn_t *conn) {
    uint8_t head[2];
    int n;
    do {
        n = recv(conn->fd, head, sizeof(head), MSG_PEEK);
    } while (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !conn->stop);
    if (n == 2 && head[1] > 4 && head[0] >= 'A' && head[0] <= 'Z') {
        ESP_LOGI(TAG, "conn %d: sender stayed in clear after pairing", conn->id);
        cipher_stream_free(&conn->control);
        conn->encrypted = false;
    }
    conn->cipher_unconfirmed = false;
}

static ssize_t conn_read(ap2_conn_t *conn, void *buf, size_t len) {
    if (conn->cipher_unconfirmed) {
        check_cipher(conn);
    }
    if (conn->encrypted) {
        return cipher_stream_read(conn->fd, &conn->control, buf, len, &conn->stop);
    }
    return ap2_recv(conn->fd, buf, len, &conn->stop);
}

static bool conn_write(ap2_conn_t *conn, const void *buf, size_t len) {
    if (conn->encrypted) {
        return cipher_stream_write(conn->fd, &conn->control, buf, len);
    }
    return write_all(conn->fd, buf, len);
}

/* Messages */

const char *rtsp_header(const rtsp_req_t *req, const char *name) {
    for (int i = 0; i < req->nheaders; i++) {
        if (!strcasecmp(req->names[i], name)) {
            return req->values[i];
        }
    }
    return NULL;
}

void rtsp_resp_header(rtsp_resp_t *resp, const char *name, const char *fmt, ...) {
    size_t room = sizeof(resp->headers) - resp->headers_len;
    int n = snprintf(resp->headers + resp->headers_len, room, "%s: ", name);
    if (n < 0 || (size_t)n >= room) {
        return;
    }
    size_t at = resp->headers_len + (size_t)n;
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(resp->headers + at, sizeof(resp->headers) - at, fmt, ap);
    va_end(ap);
    if (m < 0 || at + (size_t)m + 2 >= sizeof(resp->headers)) {
        resp->headers[resp->headers_len] = '\0';
        return;
    }
    at += (size_t)m;
    memcpy(resp->headers + at, "\r\n", 3);
    resp->headers_len = at + 2;
}

void rtsp_resp_plist(rtsp_resp_t *resp, bpw_t *w, bpw_obj root) {
    free(resp->body);
    resp->body = bpw_serialize(w, root, &resp->body_len);
    if (!resp->body) {
        resp->body_len = 0;
        resp->code = 500;
        return;
    }
    rtsp_resp_header(resp, "Content-Type", "application/x-apple-binary-plist");
}

bool rtsp_req_plist(const rtsp_req_t *req, bplist_t *pl) {
    return req->body && bplist_open(pl, req->body, req->body_len);
}

static const char *status_text(int code) {
    switch (code) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 451:
        return "Unavailable";
    case 453:
        return "Not Enough Bandwidth";
    case 456:
        return "Header Field Not Valid for Resource";
    case 470:
        return "Connection Authorization Required";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 503:
        return "Service Unavailable";
    default:
        return "Unauthorized";
    }
}

static bool send_response(ap2_conn_t *conn, rtsp_resp_t *resp) {
    char head[1024];
    int n = snprintf(head, sizeof(head), "RTSP/1.0 %d %s\r\n%sContent-Length: %u\r\n\r\n", resp->code,
                     status_text(resp->code), resp->headers, (unsigned)resp->body_len);
    if (n < 0 || (size_t)n >= sizeof(head)) {
        return false;
    }
    size_t total = (size_t)n + resp->body_len;
    uint8_t *pkt = ap2_malloc(total);
    if (!pkt) {
        return false;
    }
    memcpy(pkt, head, (size_t)n);
    if (resp->body_len) {
        memcpy(pkt + n, resp->body, resp->body_len);
    }
    bool ok = conn_write(conn, pkt, total);
    ap2_free(pkt);
    return ok;
}

static void free_request(rtsp_req_t *req) {
    ap2_free(req->head);
    heap_caps_free(req->body);
    memset(req, 0, sizeof(*req));
}

static bool parse_head(rtsp_req_t *req, char *head) {
    req->head = head;
    char *line_end = strstr(head, "\r\n");
    if (!line_end) {
        return false;
    }
    *line_end = '\0';
    if (sscanf(head, "%31s %127s", req->method, req->path) != 2) {
        return false;
    }
    for (char *line = line_end + 2; *line && req->nheaders < RTSP_MAX_HEADERS; line = line_end + 2) {
        line_end = strstr(line, "\r\n");
        if (!line_end) {
            break;
        }
        *line_end = '\0';
        char *colon = strchr(line, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';
        char *value = colon + 1;
        while (*value == ' ') {
            value++;
        }
        req->names[req->nheaders] = line;
        req->values[req->nheaders] = value;
        req->nheaders++;
    }
    return true;
}

// Reads one request. carry holds bytes received past the previous request.
static bool read_request(ap2_conn_t *conn, rtsp_req_t *req, uint8_t *carry, size_t *carry_len) {
    memset(req, 0, sizeof(*req));
    char *head = ap2_malloc(HEAD_MAX + 1);
    if (!head) {
        return false;
    }
    size_t len = *carry_len;
    memcpy(head, carry, len);
    *carry_len = 0;

    char *end;
    for (;;) {
        head[len] = '\0';
        end = len >= 4 ? strstr(head, "\r\n\r\n") : NULL;
        if (end) {
            break;
        }
        if (len == HEAD_MAX) {
            ESP_LOGW(TAG, "conn %d: request head too long", conn->id);
            ap2_free(head);
            return false;
        }
        ssize_t n = conn_read(conn, head + len, HEAD_MAX - len);
        if (n <= 0) {
            ap2_free(head);
            return false;
        }
        len += (size_t)n;
    }

    size_t head_len = (size_t)(end - head) + 4;
    size_t extra = len - head_len;
    end[2] = '\0';
    if (!parse_head(req, head)) {
        ap2_free(head);
        req->head = NULL;
        return false;
    }

    const char *cl = rtsp_header(req, "Content-Length");
    size_t body_len = cl ? strtoul(cl, NULL, 10) : 0;
    if (body_len > BODY_MAX) {
        ESP_LOGW(TAG, "conn %d: %u byte body refused", conn->id, (unsigned)body_len);
        free_request(req);
        return false;
    }
    if (body_len) {
        // Room for a NUL so text bodies can be parsed as strings.
        req->body = heap_caps_malloc(body_len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!req->body) {
            free_request(req);
            return false;
        }
        size_t have = extra < body_len ? extra : body_len;
        memcpy(req->body, head + head_len, have);
        while (have < body_len) {
            ssize_t n = conn_read(conn, req->body + have, body_len - have);
            if (n <= 0) {
                free_request(req);
                return false;
            }
            have += (size_t)n;
        }
        req->body[body_len] = '\0';
        req->body_len = body_len;
    }
    if (extra > body_len) {
        *carry_len = extra - body_len;
        memcpy(carry, head + head_len + body_len, *carry_len);
    }
    return true;
}

/* GET /info */

static int add_pstring(uint8_t *buf, size_t cap, size_t *len, const char *fmt, ...) {
    char item[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(item, sizeof(item), fmt, ap);
    va_end(ap);
    if (n < 0 || n > 255 || *len + 1 + (size_t)n > cap) {
        return -1;
    }
    buf[(*len)++] = (uint8_t)n;
    memcpy(buf + *len, item, (size_t)n);
    *len += (size_t)n;
    return 0;
}

// The _airplay._tcp TXT record, as a sequence of length-prefixed strings.
static size_t txt_airplay(uint8_t *buf, size_t cap, const ap2_conn_t *conn) {
    size_t len = 0;
    uint64_t f = ap2_features();
    const char *gid = conn && conn->group_uuid[0] ? conn->group_uuid : ap2_pi();
    int gcgl = conn ? conn->group_contains_leader : 0;
    add_pstring(buf, cap, &len, "acl=0");
    add_pstring(buf, cap, &len, "btaddr=00:00:00:00:00:00");
    add_pstring(buf, cap, &len, "deviceid=%s", ap2_device_id());
    add_pstring(buf, cap, &len, "fex=%s", ap2_fex());
    add_pstring(buf, cap, &len, "features=0x%lX,0x%lX", (unsigned long)(f & 0xFFFFFFFF), (unsigned long)(f >> 32));
    add_pstring(buf, cap, &len, "flags=0x%X", AP2_STATUS_FLAGS);
    add_pstring(buf, cap, &len, "gid=%s", gid);
    add_pstring(buf, cap, &len, "igl=0");
    add_pstring(buf, cap, &len, "gcgl=%d", gcgl);
    add_pstring(buf, cap, &len, "model=%s", AP2_MODEL);
    add_pstring(buf, cap, &len, "protovers=1.1");
    add_pstring(buf, cap, &len, "pi=%s", ap2_pi());
    add_pstring(buf, cap, &len, "psi=%s", ap2_psi());
    add_pstring(buf, cap, &len, "pk=%s", ap2_public_key_hex());
    add_pstring(buf, cap, &len, "srcvers=%s", AP2_SRCVERS);
    add_pstring(buf, cap, &len, "vv=0");
    return len;
}

bpw_obj rtsp_info_plist(bpw_t *w, const ap2_conn_t *conn) {
    char name[96];
    ap2_name(name, sizeof(name));

    bpw_obj d = bpw_dict(w);
    bpw_obj caps = bpw_dict(w);
    bpw_dict_set(w, caps, "supportsInterstitials", bpw_bool(w, false));
    bpw_dict_set(w, caps, "supportsFPSSecureStop", bpw_bool(w, false));
    bpw_dict_set(w, caps, "supportsUIForAudioOnlyContent", bpw_bool(w, false));
    bpw_dict_set(w, d, "playbackCapabilities", caps);
    bpw_dict_set(w, d, "canRecordScreenStream", bpw_bool(w, false));
    bpw_dict_set(w, d, "keepAliveSendStatsAsBody", bpw_bool(w, false));
    bpw_dict_set(w, d, "protocolVersion", bpw_string(w, "1.1"));
    bpw_dict_set(w, d, "screenDemoMode", bpw_bool(w, false));
    bpw_dict_set(w, d, "receiverHDRCapability", bpw_string(w, "4k60"));
    bpw_dict_set(w, d, "psi", bpw_string(w, ap2_psi()));
    bpw_dict_set(w, d, "featuresEx", bpw_string(w, ap2_fex()));
    bpw_dict_set(w, d, "features", bpw_uint(w, ap2_features()));
    bpw_dict_set(w, d, "statusFlags", bpw_uint(w, AP2_STATUS_FLAGS));
    bpw_dict_set(w, d, "deviceID", bpw_string(w, ap2_device_id()));
    bpw_dict_set(w, d, "pi", bpw_string(w, ap2_pi()));
    bpw_dict_set(w, d, "name", bpw_string(w, name));
    bpw_dict_set(w, d, "model", bpw_string(w, AP2_MODEL));
    bpw_dict_set(w, d, "pk", bpw_data(w, ap2_public_key(), 32));
    if (conn) {
        char sender[32];
        snprintf(sender, sizeof(sender), "%s:%u", conn->client_ip, conn->client_port);
        bpw_dict_set(w, d, "senderAddress", bpw_string(w, sender));
    }
    bpw_dict_set(w, d, "initialVolume", bpw_real(w, ap2_volume_db()));
    bpw_dict_set(w, d, "vv", bpw_uint(w, 0));
    // 3: the sender's volume slider drives this receiver's own volume.
    bpw_dict_set(w, d, "volumeControlType", bpw_uint(w, 3));

    bpw_obj formats = bpw_dict(w);
    bpw_dict_set(w, formats, "audioStream", bpw_uint(w, 0x40000));  // ALAC/44100/S16/2
    // ALAC/44100/S16/2, AAC-LC/44100/F24/2 and AAC-LC/48000/F24/2, plus
    // ALAC/48000/S24/2 for lossless (bits after shairport-sync). The DAC is
    // reclocked to whichever the sender picks.
    uint64_t buffered = 0x40000 | 0x400000 | 0x800000;
    if (ap2_lossless()) {
        buffered |= 0x200000;
    }
    bpw_dict_set(w, formats, "bufferStream", bpw_uint(w, buffered));
    bpw_dict_set(w, d, "supportedFormats", formats);

    uint8_t txt[640];
    size_t txt_len = txt_airplay(txt, sizeof(txt), conn);
    bpw_dict_set(w, d, "txtAirPlay", bpw_data(w, txt, txt_len));
    return d;
}

static void handle_get(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    if (strcmp(req->path, "/info") != 0) {
        resp->code = 501;
        return;
    }
    bpw_t *w = bpw_new();
    if (!w) {
        resp->code = 500;
        return;
    }
    resp->code = 200;
    rtsp_resp_plist(resp, w, rtsp_info_plist(w, conn));
    bpw_free(w);
}

/* Pairing */

static int pairing_add_cb(uint8_t public_key[32], const char *device_id, void *arg) {
    (void)arg;
    xSemaphoreTake(s_pairings_lock, portMAX_DELAY);
    pairing_t *p = s_pairings;
    while (p && strcmp(p->device_id, device_id) != 0) {
        p = p->next;
    }
    if (!p && (p = calloc(1, sizeof(*p)))) {
        snprintf(p->device_id, sizeof(p->device_id), "%s", device_id);
        p->next = s_pairings;
        s_pairings = p;
    }
    if (p) {
        memcpy(p->public_key, public_key, 32);
    }
    xSemaphoreGive(s_pairings_lock);
    ESP_LOGI(TAG, "paired with %s", device_id);
    return p ? 0 : -1;
}

static int pairing_remove_cb(uint8_t public_key[32], const char *device_id, void *arg) {
    (void)public_key;
    (void)arg;
    int ret = -1;
    xSemaphoreTake(s_pairings_lock, portMAX_DELAY);
    for (pairing_t **pp = &s_pairings; *pp; pp = &(*pp)->next) {
        if (!strcmp((*pp)->device_id, device_id)) {
            pairing_t *gone = *pp;
            *pp = gone->next;
            free(gone);
            ret = 0;
            break;
        }
    }
    xSemaphoreGive(s_pairings_lock);
    return ret;
}

static void pairing_list_cb(pair_cb enum_cb, void *enum_cb_arg, void *arg) {
    (void)arg;
    xSemaphoreTake(s_pairings_lock, portMAX_DELAY);
    for (pairing_t *p = s_pairings; p; p = p->next) {
        enum_cb(p->public_key, p->device_id, enum_cb_arg);
    }
    xSemaphoreGive(s_pairings_lock);
}

static void set_octet_body(rtsp_resp_t *resp, uint8_t *body, size_t len) {
    resp->body = body;
    resp->body_len = len;
    if (body) {
        rtsp_resp_header(resp, "Content-Type", "application/octet-stream");
    }
}

static void use_shared_secret(ap2_conn_t *conn, const uint8_t *secret, size_t len) {
    memcpy(conn->shared_secret, secret, len);
    conn->shared_secret_len = len;
    if (conn->pending_control_cipher) {
        pair_cipher_free(conn->pending_control_cipher);
    }
    conn->pending_control_cipher = pair_cipher_new(PAIR_SERVER_HOMEKIT, PAIR_CHANNEL_CONTROL, secret, len, "");
    if (!conn->pending_control_cipher) {
        ESP_LOGE(TAG, "conn %d: can't set up the control cipher", conn->id);
    }
}

static void handle_pair_setup(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    if (!conn->setup_ctx) {
        conn->setup_ctx = pair_setup_new(PAIR_SERVER_HOMEKIT, NULL, pairing_add_cb, NULL, ap2_device_id());
        if (!conn->setup_ctx) {
            resp->code = 500;
            return;
        }
    }
    uint8_t *body = NULL;
    size_t body_len = 0;
    if (pair_setup(&body, &body_len, conn->setup_ctx, req->body, req->body_len) < 0) {
        ESP_LOGW(TAG, "conn %d: pair-setup: %s", conn->id, pair_setup_errmsg(conn->setup_ctx));
        resp->code = 470;
    }
    set_octet_body(resp, body, body_len);

    struct pair_result *result = NULL;
    if (pair_setup_result(NULL, &result, conn->setup_ctx) == 0 && result->shared_secret_len > 0) {
        // Transient pairing is done after step 2; this reply still goes in clear.
        ESP_LOGI(TAG, "conn %d: transient pairing complete", conn->id);
        use_shared_secret(conn, result->shared_secret, result->shared_secret_len);
    }
}

static void handle_pair_verify(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    if (!conn->verify_ctx) {
        conn->verify_ctx = pair_verify_new(PAIR_SERVER_HOMEKIT, NULL, NULL, NULL, ap2_device_id());
        if (!conn->verify_ctx) {
            resp->code = 500;
            return;
        }
    }
    uint8_t *body = NULL;
    size_t body_len = 0;
    if (pair_verify(&body, &body_len, conn->verify_ctx, req->body, req->body_len) < 0) {
        ESP_LOGW(TAG, "conn %d: pair-verify: %s", conn->id, pair_verify_errmsg(conn->verify_ctx));
        resp->code = 470;
    }
    set_octet_body(resp, body, body_len);

    // shairport-sync leaves verified sessions in clear; senders that
    // pair-verify expect the control channel encrypted from here on.
    struct pair_result *result = NULL;
    if (pair_verify_result(&result, conn->verify_ctx) == 0 && result->shared_secret_len > 0) {
        ESP_LOGI(TAG, "conn %d: pair-verify complete", conn->id);
        use_shared_secret(conn, result->shared_secret, result->shared_secret_len);
    }
}

static void handle_pair_add(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    uint8_t *body = NULL;
    size_t body_len = 0;
    if (pair_add(PAIR_SERVER_HOMEKIT, &body, &body_len, pairing_add_cb, NULL, req->body, req->body_len) < 0) {
        resp->code = 451;
    }
    set_octet_body(resp, body, body_len);
    (void)conn;
}

static void handle_pair_remove(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    uint8_t *body = NULL;
    size_t body_len = 0;
    if (pair_remove(PAIR_SERVER_HOMEKIT, &body, &body_len, pairing_remove_cb, NULL, req->body, req->body_len) < 0) {
        resp->code = 451;
    }
    set_octet_body(resp, body, body_len);
    (void)conn;
}

static void handle_pair_list(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    uint8_t *body = NULL;
    size_t body_len = 0;
    if (pair_list(PAIR_SERVER_HOMEKIT, &body, &body_len, pairing_list_cb, NULL, req->body, req->body_len) < 0) {
        resp->code = 451;
    }
    set_octet_body(resp, body, body_len);
    (void)conn;
}

/* Other POSTs */

static void handle_configure(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    (void)conn;
    bpw_t *w = bpw_new();
    if (!w) {
        resp->code = 500;
        return;
    }
    bpw_obj d = bpw_dict(w);
    bplist_t pl;
    bool enable = false;
    if (rtsp_req_plist(req, &pl)) {
        bp_obj cfg = bplist_dict_get(&pl, bplist_root(&pl), "ConfigurationDictionary");
        bplist_dict_bool(&pl, cfg, "Enable_HK_Access_Control", &enable);
    }
    if (enable) {
        char name[96];
        ap2_name(name, sizeof(name));
        bpw_dict_set(w, d, "Identifier", bpw_string(w, ap2_pi()));
        bpw_dict_set(w, d, "Enable_HK_Access_Control", bpw_bool(w, true));
        bpw_dict_set(w, d, "PublicKey", bpw_data(w, ap2_public_key(), 32));
        bpw_dict_set(w, d, "Device_Name", bpw_string(w, name));
        bpw_dict_set(w, d, "Access_Control_Level", bpw_uint(w, 0));
    }
    rtsp_resp_plist(resp, w, d);
    bpw_free(w);
}

static void handle_feedback(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    (void)conn;
    (void)req;
    bpw_t *w = bpw_new();
    bpw_obj root;
    if (w && session_feedback(w, &root)) {
        rtsp_resp_plist(resp, w, root);
    }
    bpw_free(w);
}

static void handle_post(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    const char *p = req->path;
    if (!strcmp(p, "/pair-setup")) {
        handle_pair_setup(conn, req, resp);
    } else if (!strcmp(p, "/pair-verify")) {
        handle_pair_verify(conn, req, resp);
    } else if (!strcmp(p, "/pair-add")) {
        handle_pair_add(conn, req, resp);
    } else if (!strcmp(p, "/pair-remove")) {
        handle_pair_remove(conn, req, resp);
    } else if (!strcmp(p, "/pair-list")) {
        handle_pair_list(conn, req, resp);
    } else if (!strcmp(p, "/configure")) {
        handle_configure(conn, req, resp);
    } else if (!strcmp(p, "/feedback")) {
        handle_feedback(conn, req, resp);
    } else if (!strcmp(p, "/fp-setup")) {
        size_t len = 0;
        uint8_t *body = fp_setup_reply(req->body, req->body_len, &len);
        if (!body) {
            ESP_LOGW(TAG, "conn %d: unrecognised fp-setup message", conn->id);
            resp->code = 400;
        }
        set_octet_body(resp, body, len);
    } else if (strcmp(p, "/command") && strcmp(p, "/audioMode") && strcmp(p, "/pair-pin-start")) {
        ESP_LOGD(TAG, "conn %d: unhandled POST %s", conn->id, p);
    }
}

/* Parameters */

static void handle_get_parameter(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    (void)conn;
    if (req->body && strstr((const char *)req->body, "volume") == (const char *)req->body) {
        char *body = malloc(48);
        if (body) {
            resp->body_len = (size_t)snprintf(body, 48, "\r\nvolume: %.6f\r\n", ap2_volume_db());
            resp->body = (uint8_t *)body;
            rtsp_resp_header(resp, "Content-Type", "text/parameters");
        }
    }
}

static void set_parameter_text(const rtsp_req_t *req) {
    char *cp = (char *)req->body;
    while (cp && *cp) {
        char *next = strpbrk(cp, "\r\n");
        if (next) {
            *next++ = '\0';
        }
        if (!strncmp(cp, "volume: ", 8)) {
            ap2_set_volume_db(strtof(cp + 8, NULL));
        }
        cp = next;
    }
}

static void handle_set_parameter(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    (void)conn;
    (void)resp;
    const char *ct = rtsp_header(req, "Content-Type");
    if (!ct) {
        return;
    }
    if (!strncmp(ct, "text/parameters", 15)) {
        set_parameter_text(req);
    } else if (!strncmp(ct, "application/x-dmap-tagged", 25)) {
        dmap_track_t track = {0};
        if (dmap_parse_track((const char *)req->body, req->body_len, &track) &&
            (track.title[0] || track.artist[0] || track.album[0])) {
            raop_event_t ev = {.type = RAOP_EVENT_METADATA};
            ev.metadata.title = track.title;
            ev.metadata.artist = track.artist;
            ev.metadata.album = track.album;
            ap2_emit(&ev);
        }
    } else if (!strncmp(ct, "image/", 6)) {
        raop_event_t ev = {.type = RAOP_EVENT_ARTWORK};
        if (strncmp(ct, "image/none", 10) != 0) {
            ev.artwork.data = req->body;
            ev.artwork.len = req->body_len;
        }
        ap2_emit(&ev);
    }
}

/* Simple methods */

static void handle_options(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    (void)conn;
    (void)req;
    rtsp_resp_header(resp, "Public",
                     "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, FLUSHBUFFERED, TEARDOWN, OPTIONS, POST, GET, PUT");
}

static void handle_ok(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    (void)conn;
    (void)req;
    (void)resp;
}

typedef void (*handler_t)(ap2_conn_t *, const rtsp_req_t *, rtsp_resp_t *);

// Once ANNOUNCE has marked a connection AirPlay 1, these come first.
static const struct {
    const char *method;
    handler_t handler;
} s_ap1_methods[] = {
    {"OPTIONS", ap1_options},
    {"SETUP", ap1_setup},
    {"RECORD", ap1_record},
    {"FLUSH", ap1_flush},
    {"TEARDOWN", ap1_teardown},
};

static const struct {
    const char *method;
    handler_t handler;
} s_methods[] = {
    {"OPTIONS", handle_options},
    {"ANNOUNCE", ap1_announce},
    {"GET", handle_get},
    {"POST", handle_post},
    {"SETUP", session_setup},
    {"RECORD", session_record},
    {"SETRATEANCHORTIME", session_setrateanchortime},
    {"FLUSHBUFFERED", session_flushbuffered},
    {"FLUSH", session_flush},
    {"TEARDOWN", session_teardown},
    {"GET_PARAMETER", handle_get_parameter},
    {"SET_PARAMETER", handle_set_parameter},
    {"SETPEERS", handle_ok},
    {"SETPEERSX", handle_ok},
};

static void dispatch(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    // Senders of both versions name themselves on any request.
    const char *name = rtsp_header(req, "X-Apple-Client-Name");
    if (name) {
        snprintf(conn->client_name, sizeof(conn->client_name), "%s", name);
    }
    ap1_apple_challenge(conn, req, resp);
    for (size_t i = 0; conn->airplay1 && i < sizeof(s_ap1_methods) / sizeof(s_ap1_methods[0]); i++) {
        if (!strcmp(req->method, s_ap1_methods[i].method)) {
            s_ap1_methods[i].handler(conn, req, resp);
            return;
        }
    }
    for (size_t i = 0; i < sizeof(s_methods) / sizeof(s_methods[0]); i++) {
        if (!strcmp(req->method, s_methods[i].method)) {
            s_methods[i].handler(conn, req, resp);
            return;
        }
    }
    ESP_LOGD(TAG, "conn %d: unhandled %s %s", conn->id, req->method, req->path);
    resp->code = 501;
}

/* Connections */

static void conn_free(ap2_conn_t *conn) {
    if (conn->events) {
        event_close(conn->events);
    }
    if (conn->setup_ctx) {
        pair_setup_free(conn->setup_ctx);
    }
    if (conn->verify_ctx) {
        pair_verify_free(conn->verify_ctx);
    }
    if (conn->pending_control_cipher) {
        pair_cipher_free(conn->pending_control_cipher);
    }
    cipher_stream_free(&conn->control);
    ap1_free(conn);
    if (conn->fd >= 0) {
        close(conn->fd);
    }
    memset(conn->shared_secret, 0, sizeof(conn->shared_secret));
    free(conn);
}

static void conn_task(void *arg) {
    ap2_conn_t *conn = arg;
    uint8_t *carry = ap2_malloc(HEAD_MAX);
    size_t carry_len = 0;
    rtsp_req_t req;

    ESP_LOGI(TAG, "conn %d: from %s:%u", conn->id, conn->client_ip, conn->client_port);
    while (carry && !conn->stop && read_request(conn, &req, carry, &carry_len)) {
        rtsp_resp_t resp = {.code = 200};
        const char *cseq = rtsp_header(&req, "CSeq");
        if (cseq) {
            rtsp_resp_header(&resp, "CSeq", "%s", cseq);
        }
        rtsp_resp_header(&resp, "Server", "AirTunes/%s", AP2_SRCVERS);
        ESP_LOGD(TAG, "conn %d: %s %s (%u bytes)", conn->id, req.method, req.path, (unsigned)req.body_len);

        dispatch(conn, &req, &resp);
        if (conn->airplay1) {
            rtsp_resp_header(&resp, "Audio-Jack-Status", "connected; type=analog");
        }
        bool ok = send_response(conn, &resp);
        free(resp.body);
        free_request(&req);
        if (!ok) {
            ESP_LOGW(TAG, "conn %d: can't send a response", conn->id);
            break;
        }
        if (conn->pending_control_cipher) {
            cipher_stream_free(&conn->control);
            conn->control.ctx = conn->pending_control_cipher;
            conn->pending_control_cipher = NULL;
            conn->encrypted = true;
            conn->cipher_unconfirmed = true;
        }
    }
    ap2_free(carry);
    ESP_LOGI(TAG, "conn %d: closed", conn->id);

    session_conn_closed(conn);
    xSemaphoreTake(s_conns_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CONNS; i++) {
        if (s_conns[i] == conn) {
            s_conns[i] = NULL;
        }
    }
    xSemaphoreGive(s_conns_lock);
    conn_free(conn);
    ap2_task_exit();
}

static void accept_conn(int fd, const struct sockaddr_in *from) {
    ap2_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn) {
        close(fd);
        return;
    }
    conn->fd = fd;
    inet_ntoa_r(from->sin_addr, conn->client_ip, sizeof(conn->client_ip));
    conn->client_port = ntohs(from->sin_port);
    struct sockaddr_in self;
    socklen_t self_len = sizeof(self);
    if (getsockname(fd, (struct sockaddr *)&self, &self_len) == 0) {
        inet_ntoa_r(self.sin_addr, conn->self_ip, sizeof(conn->self_ip));
    }
    struct timeval tv = {.tv_sec = RECV_TIMEOUT_MS / 1000, .tv_usec = (RECV_TIMEOUT_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    xSemaphoreTake(s_conns_lock, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_CONNS && slot < 0; i++) {
        if (!s_conns[i]) {
            slot = i;
        }
    }
    if (slot >= 0) {
        conn->id = s_next_id++;
        s_conns[slot] = conn;
    }
    xSemaphoreGive(s_conns_lock);

    if (slot < 0) {
        ESP_LOGW(TAG, "too many connections, refusing %s", conn->client_ip);
        conn_free(conn);
        return;
    }
    char name[16];
    snprintf(name, sizeof(name), "ap2_rtsp%d", conn->id);
    if (!ap2_task_start(conn_task, name, CONN_STACK, conn, CONN_PRIO, 0, true)) {
        xSemaphoreTake(s_conns_lock, portMAX_DELAY);
        s_conns[slot] = NULL;
        xSemaphoreGive(s_conns_lock);
        conn_free(conn);
    }
}

static void listen_task(void *arg) {
    int lfd = (int)(intptr_t)arg;
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int fd = accept(lfd, (struct sockaddr *)&from, &from_len);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        accept_conn(fd, &from);
    }
}

esp_err_t rtsp_start(void) {
    s_conns_lock = xSemaphoreCreateMutex();
    s_pairings_lock = xSemaphoreCreateMutex();
    if (!s_conns_lock || !s_pairings_lock) {
        return ESP_ERR_NO_MEM;
    }
    int lfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (lfd < 0) {
        return ESP_FAIL;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(AP2_RTSP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(lfd, MAX_CONNS) < 0) {
        close(lfd);
        return ESP_FAIL;
    }
    if (!ap2_task_start(listen_task, "ap2_listen", LISTEN_STACK, (void *)(intptr_t)lfd, LISTEN_PRIO, 0, true)) {
        close(lfd);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void rtsp_disconnect_all(void) {
    xSemaphoreTake(s_conns_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_CONNS; i++) {
        if (s_conns[i]) {
            s_conns[i]->stop = true;
            // Wakes a blocked recv() at once.
            shutdown(s_conns[i]->fd, SHUT_RDWR);
        }
    }
    xSemaphoreGive(s_conns_lock);
}
