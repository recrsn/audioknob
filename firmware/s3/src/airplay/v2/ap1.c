/*
 * AirPlay 1 (RAOP) sessions on the AirPlay 2 listener, as shairport-sync
 * takes them in its AirPlay 2 build: ANNOUNCE marks the connection as
 * AirPlay 1, and its SETUP, RECORD, FLUSH and TEARDOWN then go here rather
 * than to session.c's AirPlay 2 handlers, behaving as shairport-sync's
 * classic handlers do (rtsp.c, Copyright (c) James Laird 2013, Mike Brady
 * 2014--2026, MIT License). Volume, metadata and artwork arrive as
 * SET_PARAMETER in both protocols and share rtsp.c's handler.
 */

#include "ap1.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ap2.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "raop_setup.h"
#include "session.h"

static const char *TAG = "ap2_ap1";

void ap1_apple_challenge(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    const char *challenge = rtsp_header(req, "Apple-Challenge");
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    // Signed over the address the sender actually reached.
    if (!challenge || getsockname(conn->fd, (struct sockaddr *)&local, &len) != 0) {
        return;
    }
    char *response = raop_auth_challenge(challenge, local.sin_addr, ap2_mac());
    if (response) {
        rtsp_resp_header(resp, "Apple-Response", "%s", response);
        free(response);
    }
}

void ap1_free(ap2_conn_t *conn) {
    free(conn->ap1.rtpmap);
    free(conn->ap1.fmtp);
    memset(&conn->ap1, 0, sizeof(conn->ap1));
}

static void copy_header(const rtsp_req_t *req, const char *name, char *out, size_t len) {
    const char *value = rtsp_header(req, name);
    snprintf(out, len, "%s", value ? value : "");
}

void ap1_announce(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    const char *sdp = (const char *)req->body;
    ap1_free(conn);
    conn->airplay1 = true;
    if (!sdp) {
        resp->code = 400;
        return;
    }

    // Both or neither: a stream can't be half encrypted.
    char *key = raop_sdp_attr(sdp, "rsaaeskey");
    char *iv = raop_sdp_attr(sdp, "aesiv");
    if (!key != !iv) {
        ESP_LOGW(TAG, "conn %d: ANNOUNCE has only one of rsaaeskey and aesiv", conn->id);
        resp->code = 456;
    } else if (key) {
        conn->ap1.encrypted = raop_auth_aes_key(key, conn->ap1.aeskey) &&
                              raop_base64_decode(iv, conn->ap1.aesiv, sizeof(conn->ap1.aesiv)) == 16;
        if (!conn->ap1.encrypted) {
            ESP_LOGE(TAG, "conn %d: can't recover the AES key", conn->id);
            resp->code = 456;
        }
    }
    free(key);
    free(iv);
    conn->ap1.rtpmap = raop_sdp_attr(sdp, "rtpmap");
    conn->ap1.fmtp = raop_sdp_attr(sdp, "fmtp");
    copy_header(req, "DACP-ID", conn->ap1.dacp_id, sizeof(conn->ap1.dacp_id));
    copy_header(req, "Active-Remote", conn->ap1.active_remote, sizeof(conn->ap1.active_remote));
    ESP_LOGI(TAG, "conn %d: AirPlay 1 sender", conn->id);
}

void ap1_setup(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    uint16_t control_port, timing_port, ports[3];
    if (!raop_parse_transport(rtsp_header(req, "Transport"), &control_port, &timing_port)) {
        ESP_LOGW(TAG, "conn %d: SETUP without control/timing ports", conn->id);
        resp->code = 400;
        return;
    }
    if (!session_ap1_setup(conn, control_port, timing_port, ports)) {
        resp->code = 503;
        return;
    }
    rtsp_resp_header(resp, "Transport",
                     "RTP/AVP/UDP;unicast;interleaved=0-1;mode=record;control_port=%u;timing_port=%u;server_port=%u",
                     ports[0], ports[1], ports[2]);
    rtsp_resp_header(resp, "Session", "1");
}

void ap1_options(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    (void)conn;
    (void)req;
    rtsp_resp_header(resp, "Public", "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER");
}

void ap1_record(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    // The minimum latency, which the sender adds its own to.
    rtsp_resp_header(resp, "Audio-Latency", "11025");
    uint16_t seqno;
    uint32_t rtptime;
    raop_parse_rtp_info(rtsp_header(req, "RTP-Info"), &seqno, &rtptime);
    session_ap1_record(conn, seqno, rtptime);
}

void ap1_flush(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    uint16_t seqno;
    uint32_t rtptime;
    raop_parse_rtp_info(rtsp_header(req, "RTP-Info"), &seqno, &rtptime);
    if (!session_ap1_flush(conn, seqno, rtptime)) {
        resp->code = 451;
    }
}

void ap1_teardown(ap2_conn_t *conn, const rtsp_req_t *req, rtsp_resp_t *resp) {
    (void)req;
    session_ap1_teardown(conn);
    rtsp_resp_header(resp, "Connection", "close");
    conn->stop = true;
}
