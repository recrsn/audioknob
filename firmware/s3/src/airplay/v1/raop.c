/*
 * AirPlay 1 (RAOP) receiver: mDNS advertisement, RTSP session handling,
 * and DACP remote control back to the sender.
 *
 * Ported from philippe44's squeezelite-esp32 raop component
 * (c) Philippe 2019-2020, philippe_44@outlook.com -- MIT License,
 * https://opensource.org/licenses/MIT
 */

#ifndef AK_NATIVE

#include "raop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dacp.h"
#include "dmap.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "http_util.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "raop_setup.h"
#include "raop_rtp.h"

#define RTSP_PORT 5000
#define RTSP_STACK_SIZE 8192  // RSA private-key ops are stack-hungry
#define RTSP_TASK_PRIO 5
#define MAX_HEADERS 16
#define MAX_RESP_HEADERS 8

struct raop_ctx_s {
    int sock;
    struct in_addr peer;
    volatile bool abort;
    uint8_t mac[6];
    struct {
        uint8_t aeskey[16], aesiv[16];
        bool has_key, has_iv;
        char *rtpmap, *fmtp;
    } rtsp;
    raop_rtp_t *rtp;
    bool session_active;
    volatile bool enabled;
    char instance[96];
    raop_event_cb_t event_cb;
    raop_pcm_cb_t pcm_cb;
};

static const char *TAG = "raop";

static void rtsp_thread(void *arg);

uint32_t raop_time_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void emit(raop_ctx_t *ctx, raop_event_type_t type) {
    raop_event_t event = {.type = type};
    ctx->event_cb(&event);
}

static void clear_stream_params(raop_ctx_t *ctx) {
    ctx->rtsp.has_key = ctx->rtsp.has_iv = false;
    free(ctx->rtsp.rtpmap);
    free(ctx->rtsp.fmtp);
    ctx->rtsp.rtpmap = ctx->rtsp.fmtp = NULL;
}

static void end_session(raop_ctx_t *ctx) {
    if (ctx->rtp) {
        raop_rtp_end(ctx->rtp);
        ctx->rtp = NULL;
    }
    dacp_stop();
    clear_stream_params(ctx);

    if (ctx->session_active) {
        ctx->session_active = false;
        emit(ctx, RAOP_EVENT_SESSION_END);
    }
}

static void handle_announce(raop_ctx_t *ctx, const key_data_t *headers, const char *body) {
    clear_stream_params(ctx);

    if (body) {
        char *value;
        if ((value = raop_sdp_attr(body, "rsaaeskey"))) {
            ctx->rtsp.has_key = raop_auth_aes_key(value, ctx->rtsp.aeskey);
            if (!ctx->rtsp.has_key) {
                ESP_LOGE(TAG, "cannot decrypt AES key");
            }
            free(value);
        }
        if ((value = raop_sdp_attr(body, "aesiv"))) {
            ctx->rtsp.has_iv = raop_base64_decode(value, ctx->rtsp.aesiv, sizeof(ctx->rtsp.aesiv)) == 16;
            free(value);
        }
        ctx->rtsp.rtpmap = raop_sdp_attr(body, "rtpmap");
        ctx->rtsp.fmtp = raop_sdp_attr(body, "fmtp");
    }

    dacp_start(kd_lookup(headers, "DACP-ID"), kd_lookup(headers, "Active-Remote"));
}

static bool handle_setup(raop_ctx_t *ctx, const key_data_t *headers, key_data_t *resp) {
    uint16_t cport, tport;
    if (!raop_parse_transport(kd_lookup(headers, "Transport"), &cport, &tport)) {
        ESP_LOGW(TAG, "SETUP without control/timing ports");
        return false;
    }

    if (!ctx->enabled) {
        return false;
    }
    if (!ctx->session_active) {
        raop_event_t event = {.type = RAOP_EVENT_SESSION_START,
                              .session.client = kd_lookup(headers, "X-Apple-Client-Name")};
        if (!ctx->event_cb(&event)) {
            return false;
        }
        ctx->session_active = true;
    }

    if (ctx->rtp) {
        raop_rtp_end(ctx->rtp);
    }
    bool encrypted = ctx->rtsp.has_key && ctx->rtsp.has_iv;
    raop_rtp_resp_t rtp = raop_rtp_init(ctx->peer, encrypted ? ctx->rtsp.aeskey : NULL, encrypted ? ctx->rtsp.aesiv : NULL,
                              ctx->rtsp.rtpmap, ctx->rtsp.fmtp, cport, tport, ctx->event_cb, ctx->pcm_cb);
    ctx->rtp = rtp.ctx;
    if (!rtp.ctx) {
        return false;
    }

    char value[128];
    snprintf(value, sizeof(value), "RTP/AVP/UDP;unicast;mode=record;control_port=%u;timing_port=%u;server_port=%u",
             rtp.cport, rtp.tport, rtp.aport);
    kd_add(resp, "Transport", value);
    kd_add(resp, "Session", "DEADBEEF");
    return true;
}

static void handle_set_parameter(raop_ctx_t *ctx, const key_data_t *headers, const char *body, int len) {
    const char *ctype = kd_lookup(headers, "Content-Type");
    const char *p;

    // Senders mark a track without art as image/none, usually with no body.
    if (ctype && !strcasecmp(ctype, "image/none")) {
        raop_event_t event = {.type = RAOP_EVENT_ARTWORK};
        ctx->event_cb(&event);
        return;
    }
    if (!body) {
        return;
    }
    ESP_LOGI(TAG, "SET_PARAMETER %s, %d bytes", ctype ? ctype : "(no type)", len);

    if (ctype && !strcasecmp(ctype, "application/x-dmap-tagged")) {
        dmap_track_t track = {0};
        if (dmap_parse_track(body, len, &track)) {
            raop_event_t event = {
                .type = RAOP_EVENT_METADATA,
                .metadata = {.title = track.title, .artist = track.artist, .album = track.album},
            };
            ctx->event_cb(&event);
        }
    } else if (ctype && !strncasecmp(ctype, "image/", 6)) {
        // JPEG from iOS, often PNG from macOS Music; the decoder sniffs which.
        raop_event_t event = {.type = RAOP_EVENT_ARTWORK, .artwork = {.data = (const uint8_t *)body, .len = len}};
        ctx->event_cb(&event);
    } else if ((p = strcasestr(body, "volume:"))) {
        float db;
        if (sscanf(p + 7, "%f", &db) == 1) {
            // -144 dB means mute; otherwise the slider spans -30..0 dB.
            float volume = db <= -144.0f ? 0.0f : 1.0f + db / 30.0f;
            raop_event_t event = {
                .type = RAOP_EVENT_VOLUME,
                .volume = volume < 0.0f ? 0.0f : (volume > 1.0f ? 1.0f : volume),
            };
            ctx->event_cb(&event);
        }
    }
}

static bool handle_rtsp(raop_ctx_t *ctx, int sock) {
    key_data_t headers[MAX_HEADERS + 1];
    key_data_t resp[MAX_RESP_HEADERS + 1] = {{0}};
    char method[16];
    char *body = NULL;
    int len = 0;
    bool success = true;
    const char *value;

    if (!http_parse(sock, method, sizeof(method), headers, MAX_HEADERS, &body, &len)) {
        return false;
    }
    if (strcmp(method, "OPTIONS")) {
        ESP_LOGI(TAG, "received %s", method);
    }

    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    if ((value = kd_lookup(headers, "Apple-Challenge")) &&
        getsockname(sock, (struct sockaddr *)&local, &local_len) == 0) {
        // Signed over the IP the sender actually reached, not whatever DHCP said last.
        char *response = raop_auth_challenge(value, local.sin_addr, ctx->mac);
        if (response) {
            kd_add(resp, "Apple-Response", response);
            free(response);
        }
    }

    if (!strcmp(method, "OPTIONS")) {
        kd_add(resp, "Public",
               "ANNOUNCE, SETUP, RECORD, PAUSE, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER");
    } else if (!strcmp(method, "ANNOUNCE")) {
        handle_announce(ctx, headers, body);
    } else if (!strcmp(method, "SETUP")) {
        success = handle_setup(ctx, headers, resp);
    } else if (!strcmp(method, "RECORD")) {
        uint16_t seqno;
        uint32_t rtptime;
        raop_parse_rtp_info(kd_lookup(headers, "RTP-Info"), &seqno, &rtptime);
        if (ctx->rtp) {
            raop_rtp_record(ctx->rtp, seqno, rtptime);
        }
    } else if (!strcmp(method, "FLUSH")) {
        uint16_t seqno;
        uint32_t rtptime;
        raop_parse_rtp_info(kd_lookup(headers, "RTP-Info"), &seqno, &rtptime);
        if (ctx->rtp) {
            raop_rtp_flush(ctx->rtp, seqno, rtptime);
        }
    } else if (!strcmp(method, "TEARDOWN")) {
        end_session(ctx);
    } else if (!strcmp(method, "SET_PARAMETER")) {
        handle_set_parameter(ctx, headers, body, len);
    }

    kd_add(resp, "Audio-Jack-Status", "connected; type=analog");
    if ((value = kd_lookup(headers, "CSeq"))) {
        kd_add(resp, "CSeq", value);
    }
    bool sent = http_send(sock, success ? "RTSP/1.0 200 OK" : "RTSP/1.0 503 Service Unavailable", resp);

    free(body);
    kd_free(resp);
    kd_free(headers);
    return success && sent;
}

static void rtsp_thread(void *arg) {
    raop_ctx_t *ctx = arg;
    int sock = -1;

    for (;;) {
        if (sock < 0) {
            struct sockaddr_in peer;
            socklen_t addr_len = sizeof(peer);
            sock = accept(ctx->sock, (struct sockaddr *)&peer, &addr_len);
            if (sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            ctx->peer = peer.sin_addr;
            ctx->abort = false;
            ESP_LOGI(TAG, "RTSP connection from %s", inet_ntoa(ctx->peer));
        }

        fd_set rfds;
        struct timeval timeout = {0, 100 * 1000};
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);

        int n = select(sock + 1, &rfds, NULL, NULL, &timeout);
        if (n == 0 && !ctx->abort) {
            continue;
        }

        bool ok = n > 0 && handle_rtsp(ctx, sock);
        if (!ok || ctx->abort) {
            end_session(ctx);
            close(sock);
            ESP_LOGI(TAG, "RTSP connection closed");
            sock = -1;
        }
    }
}

// AirPlay 1 instance names are "<MAC>@<speaker name>".
static void instance_name(const raop_ctx_t *ctx, const char *name, char *out, size_t len) {
    const uint8_t *mac = ctx->mac;
    snprintf(out, len, "%02X%02X%02X%02X%02X%02X@%s", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], name);
}

// Advertised capabilities: ALAC only (no raw PCM decoder here) at
// 44.1k/16/stereo over UDP, RSA-encrypted or clear, text metadata and
// JPEG/PNG artwork (no progress).
static esp_err_t advertise(const raop_ctx_t *ctx) {
    const mdns_txt_item_t txt[] = {
        {"txtvers", "1"}, {"am", "AudioKnob"}, {"tp", "UDP"},  {"sm", "false"}, {"sv", "false"},
        {"ek", "1"},      {"et", "0,1"},       {"md", "0,1"},    {"cn", "1"},   {"ch", "2"},
        {"ss", "16"},     {"sr", "44100"},     {"vn", "3"},
    };
    return mdns_service_add(ctx->instance, "_raop", "_tcp", RTSP_PORT, (mdns_txt_item_t *)txt,
                            sizeof(txt) / sizeof(txt[0]));
}

raop_ctx_t *raop_create(const char *name, const uint8_t mac[6], uint8_t protocols, raop_event_cb_t event_cb,
                        raop_pcm_cb_t pcm_cb) {
    raop_ctx_t *ctx = calloc(1, sizeof(raop_ctx_t));
    if (!ctx) {
        return NULL;
    }
    ctx->sock = -1;
    memcpy(ctx->mac, mac, 6);
    ctx->event_cb = event_cb;
    ctx->pcm_cb = pcm_cb;
    dacp_init();
    if (!raop_setup_init()) {
        goto fail;
    }

    ctx->sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(RTSP_PORT),
    };
    if (ctx->sock < 0 || bind(ctx->sock, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(ctx->sock, 1) < 0) {
        ESP_LOGE(TAG, "cannot bind/listen RTSP: %s", strerror(errno));
        goto fail;
    }

    instance_name(ctx, name, ctx->instance, sizeof(ctx->instance));
    ctx->enabled = protocols & RAOP_PROTOCOL_AIRPLAY1;
    if (ctx->enabled && advertise(ctx) != ESP_OK) {
        ESP_LOGE(TAG, "cannot register mDNS service");
        goto fail;
    }

    if (xTaskCreatePinnedToCore(rtsp_thread, "rtsp", RTSP_STACK_SIZE, ctx, RTSP_TASK_PRIO, NULL, tskNO_AFFINITY) !=
        pdPASS) {
        mdns_service_remove("_raop", "_tcp");
        goto fail;
    }

    ESP_LOGI(TAG, "AirPlay receiver \"%s\" listening on port %d", name, RTSP_PORT);
    return ctx;

fail:
    if (ctx->sock >= 0) {
        close(ctx->sock);
    }
    free(ctx);
    return NULL;
}

void raop_set_protocols(raop_ctx_t *ctx, uint8_t protocols) {
    bool enabled = protocols & RAOP_PROTOCOL_AIRPLAY1;
    if (enabled == ctx->enabled) {
        return;
    }
    ctx->enabled = enabled;
    if (enabled) {
        if (advertise(ctx) != ESP_OK) {
            ESP_LOGE(TAG, "cannot register mDNS service");
        }
    } else {
        mdns_service_remove("_raop", "_tcp");
        ctx->abort = true;
    }
    ESP_LOGI(TAG, "AirPlay %s", enabled ? "on" : "off");
}

void raop_abort(raop_ctx_t *ctx) {
    ctx->abort = true;
}

bool raop_remote_command(raop_ctx_t *ctx, const char *command) {
    (void)ctx;
    return dacp_command(command);
}

bool raop_remote_volume(raop_ctx_t *ctx, float volume) {
    (void)ctx;
    return dacp_volume(volume);
}

void raop_set_name(raop_ctx_t *ctx, const char *name) {
    instance_name(ctx, name, ctx->instance, sizeof(ctx->instance));
    if (!ctx->enabled) {
        return;
    }
    if (mdns_service_instance_name_set("_raop", "_tcp", ctx->instance) != ESP_OK) {
        ESP_LOGW(TAG, "cannot rename mDNS service");
        return;
    }
    ESP_LOGI(TAG, "AirPlay receiver renamed \"%s\"", name);
}

#endif  // !AK_NATIVE
