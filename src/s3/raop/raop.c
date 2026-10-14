/*
 * AirPlay 1 (RAOP) receiver: mDNS advertisement, RTSP session handling,
 * and DACP remote control back to the sender.
 *
 * Ported from philippe44's squeezelite-esp32 raop component
 * (c) Philippe 2019-2020, philippe_44@outlook.com -- MIT License,
 * https://opensource.org/licenses/MIT
 */

#include "raop.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "dmap.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_util.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mdns.h"
#include "rtp.h"

#define RTSP_PORT 5000
#define RTSP_STACK_SIZE 8192  // RSA private-key ops are stack-hungry
#define RTSP_TASK_PRIO 5
#define REMOTE_STACK_SIZE 4096
#define REMOTE_TASK_PRIO 3
#define REMOTE_TIMEOUT_S 2
#define MAX_HEADERS 16
#define MAX_RESP_HEADERS 8

struct raop_ctx_s {
    int sock;
    struct in_addr peer;
    volatile bool abort;
    uint8_t mac[6];
    mbedtls_pk_context pk;
    struct {
        uint8_t aeskey[16], aesiv[16];
        bool has_key, has_iv;
        char *fmtp;
    } rtsp;
    rtp_t *rtp;
    bool session_active;
    raop_event_cb_t event_cb;
    raop_pcm_cb_t pcm_cb;
    struct {
        char dacp_id[32], active_remote[32];
        struct in_addr host;
        uint16_t port;
        volatile bool searching;
        TaskHandle_t task;
        SemaphoreHandle_t done, lock;
    } remote;
};

static const char *TAG = "raop";

// The AirPort Express private key that every AirPlay 1 sender authenticates
// receivers against; public knowledge since 2011 (shairport et al.).
static const char airport_rsa_key[] =
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt\n"
    "wC5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDRKSKv6kDqnw4U\n"
    "wPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf\n"
    "/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/\n"
    "UAaHqn9JdsBWLUEpVviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hEewIDAQABAoIBAQDl8Axy9XfW\n"
    "BLmkzkEiqoSwF0PsmVrPzH9KsnwLGH+QZlvjWd8SWYGN7u1507HvhF5N3drJoVU3O14nDY4TFQAa\n"
    "LlJ9VM35AApXaLyY1ERrN7u9ALKd2LUwYhM7Km539O4yUFYikE2nIPscEsA5ltpxOgUGCY7b7ez5\n"
    "NtD6nL1ZKauw7aNXmVAvmJTcuPxWmoktF3gDJKK2wxZuNGcJE0uFQEG4Z3BrWP7yoNuSK3dii2jm\n"
    "lpPHr0O/KnPQtzI3eguhe0TwUem/eYSdyzMyVx/YpwkzwtYL3sR5k0o9rKQLtvLzfAqdBxBurciz\n"
    "aaA/L0HIgAmOit1GJA2saMxTVPNhAoGBAPfgv1oeZxgxmotiCcMXFEQEWflzhWYTsXrhUIuz5jFu\n"
    "a39GLS99ZEErhLdrwj8rDDViRVJ5skOp9zFvlYAHs0xh92ji1E7V/ysnKBfsMrPkk5KSKPrnjndM\n"
    "oPdevWnVkgJ5jxFuNgxkOLMuG9i53B4yMvDTCRiIPMQ++N2iLDaRAoGBAO9v//mU8eVkQaoANf0Z\n"
    "oMjW8CN4xwWA2cSEIHkd9AfFkftuv8oyLDCG3ZAf0vrhrrtkrfa7ef+AUb69DNggq4mHQAYBp7L+\n"
    "k5DKzJrKuO0r+R0YbY9pZD1+/g9dVt91d6LQNepUE/yY2PP5CNoFmjedpLHMOPFdVgqDzDFxU8hL\n"
    "AoGBANDrr7xAJbqBjHVwIzQ4To9pb4BNeqDndk5Qe7fT3+/H1njGaC0/rXE0Qb7q5ySgnsCb3DvA\n"
    "cJyRM9SJ7OKlGt0FMSdJD5KG0XPIpAVNwgpXXH5MDJg09KHeh0kXo+QA6viFBi21y340NonnEfdf\n"
    "54PX4ZGS/Xac1UK+pLkBB+zRAoGAf0AY3H3qKS2lMEI4bzEFoHeK3G895pDaK3TFBVmD7fV0Zhov\n"
    "17fegFPMwOII8MisYm9ZfT2Z0s5Ro3s5rkt+nvLAdfC/PYPKzTLalpGSwomSNYJcB9HNMlmhkGzc\n"
    "1JnLYT4iyUyx6pcZBmCd8bD0iwY/FzcgNDaUmbX9+XDvRA0CgYEAkE7pIPlE71qvfJQgoA9em0gI\n"
    "LAuE4Pu13aKiJnfft7hIjbK+5kyb3TysZvoyDnb3HOKvInK7vXbKuU4ISgxB2bB3HcYzQMGsz1qJ\n"
    "2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=\n"
    "-----END RSA PRIVATE KEY-----";

static void rtsp_thread(void *arg);

uint32_t raop_time_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int rng(void *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    esp_fill_random(buf, len);
    return 0;
}

static void emit(raop_ctx_t *ctx, raop_event_type_t type) {
    raop_event_t event = {.type = type};
    ctx->event_cb(&event);
}

// Apple senders omit base64 padding; mbedtls requires it.
static size_t base64_decode(const char *src, uint8_t *dst, size_t dst_size) {
    size_t len = strlen(src);
    size_t padded_len = (len + 3) & ~(size_t)3;
    char *padded = malloc(padded_len + 1);
    size_t out_len = 0;

    if (padded) {
        memcpy(padded, src, len);
        memset(padded + len, '=', padded_len - len);
        padded[padded_len] = '\0';
        if (mbedtls_base64_decode(dst, dst_size, &out_len, (const uint8_t *)padded, padded_len) != 0) {
            out_len = 0;
        }
        free(padded);
    }
    return out_len;
}

// Value of an SDP "a=<name>:" attribute, up to the line ending. Heap-owned.
static char *sdp_attr(const char *body, const char *name) {
    char needle[24];
    snprintf(needle, sizeof(needle), "a=%s:", name);
    const char *p = strcasestr(body, needle);
    if (!p) {
        return NULL;
    }
    p += strlen(needle);
    return strndup(p, strcspn(p, "\r\n"));
}

static char *challenge_response(raop_ctx_t *ctx, int sock, const char *challenge) {
    uint8_t data[32] = {0};
    size_t n = base64_decode(challenge, data, 16);

    // The IP the sender actually reached us on, not whatever DHCP said last.
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    if (n == 0 || getsockname(sock, (struct sockaddr *)&local, &len) != 0) {
        return NULL;
    }
    memcpy(data + n, &local.sin_addr.s_addr, 4);
    memcpy(data + n + 4, ctx->mac, 6);

    // "Encrypt with the private key": a PKCS#1 v1.5 type-1 signature over
    // the raw 32 bytes, no digest.
    uint8_t sig[256];
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(ctx->pk);
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
    if (mbedtls_rsa_pkcs1_sign(rsa, rng, NULL, MBEDTLS_MD_NONE, sizeof(data), data, sig) != 0) {
        ESP_LOGE(TAG, "Apple-Challenge signing failed");
        return NULL;
    }

    char b64[352];
    size_t b64_len = 0;
    if (mbedtls_base64_encode((uint8_t *)b64, sizeof(b64), &b64_len, sig, sizeof(sig)) != 0) {
        return NULL;
    }
    while (b64_len > 0 && b64[b64_len - 1] == '=') {
        b64[--b64_len] = '\0';
    }
    return strdup(b64);
}

static bool decrypt_aes_key(raop_ctx_t *ctx, const char *b64, uint8_t out[16]) {
    uint8_t encrypted[256 + 4];
    size_t len = base64_decode(b64, encrypted, sizeof(encrypted));
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(ctx->pk);
    if (len != mbedtls_rsa_get_len(rsa)) {
        return false;
    }

    uint8_t plain[256];
    size_t plain_len = 0;
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
    if (mbedtls_rsa_pkcs1_decrypt(rsa, rng, NULL, &plain_len, encrypted, plain, sizeof(plain)) != 0 ||
        plain_len != 16) {
        return false;
    }
    memcpy(out, plain, 16);
    return true;
}

static void remote_search_task(void *arg) {
    raop_ctx_t *ctx = arg;
    bool found = false;

    while (ctx->remote.searching && !found) {
        mdns_result_t *results = NULL;
        if (mdns_query_ptr("_dacp", "_tcp", 3000, 32, &results) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        for (mdns_result_t *r = results; r && !found; r = r->next) {
            if (!r->instance_name || !strcasestr(r->instance_name, ctx->remote.dacp_id)) {
                continue;
            }
            for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
                if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                    xSemaphoreTake(ctx->remote.lock, portMAX_DELAY);
                    ctx->remote.host.s_addr = a->addr.u_addr.ip4.addr;
                    ctx->remote.port = r->port;
                    xSemaphoreGive(ctx->remote.lock);
                    found = true;
                    ESP_LOGI(TAG, "found DACP remote %s at %s:%u", r->instance_name, inet_ntoa(ctx->remote.host),
                             r->port);
                    break;
                }
            }
        }
        mdns_query_results_free(results);
    }

    xSemaphoreGive(ctx->remote.done);
    vTaskDelete(NULL);
}

static void remote_stop(raop_ctx_t *ctx) {
    if (ctx->remote.task) {
        ctx->remote.searching = false;
        // Bounded by the in-flight mDNS query's timeout.
        xSemaphoreTake(ctx->remote.done, portMAX_DELAY);
        ctx->remote.task = NULL;
    }
    xSemaphoreTake(ctx->remote.lock, portMAX_DELAY);
    ctx->remote.port = 0;
    ctx->remote.host.s_addr = 0;
    xSemaphoreGive(ctx->remote.lock);
}

static void remote_start(raop_ctx_t *ctx, const char *dacp_id, const char *active_remote) {
    remote_stop(ctx);
    if (!dacp_id || !active_remote) {
        return;
    }

    snprintf(ctx->remote.dacp_id, sizeof(ctx->remote.dacp_id), "%s", dacp_id);
    snprintf(ctx->remote.active_remote, sizeof(ctx->remote.active_remote), "%s", active_remote);
    ctx->remote.searching = true;
    if (xTaskCreatePinnedToCore(remote_search_task, "dacp_search", REMOTE_STACK_SIZE, ctx, REMOTE_TASK_PRIO,
                                &ctx->remote.task, tskNO_AFFINITY) != pdPASS) {
        ctx->remote.searching = false;
        ctx->remote.task = NULL;
    }
}

static void clear_stream_params(raop_ctx_t *ctx) {
    ctx->rtsp.has_key = ctx->rtsp.has_iv = false;
    free(ctx->rtsp.fmtp);
    ctx->rtsp.fmtp = NULL;
}

static void end_session(raop_ctx_t *ctx) {
    if (ctx->rtp) {
        rtp_end(ctx->rtp);
        ctx->rtp = NULL;
    }
    remote_stop(ctx);
    clear_stream_params(ctx);

    if (ctx->session_active) {
        ctx->session_active = false;
        emit(ctx, RAOP_EVENT_SESSION_END);
    }
}

static void parse_rtp_info(const key_data_t *headers, uint16_t *seqno, uint32_t *rtptime) {
    const char *info = kd_lookup(headers, "RTP-Info");
    const char *p;
    unsigned long value;

    *seqno = 0;
    *rtptime = 0;
    if (info && (p = strcasestr(info, "seq=")) && sscanf(p + 4, "%lu", &value) == 1) {
        *seqno = value;
    }
    if (info && (p = strcasestr(info, "rtptime=")) && sscanf(p + 8, "%lu", &value) == 1) {
        *rtptime = value;
    }
}

static void handle_announce(raop_ctx_t *ctx, const key_data_t *headers, const char *body) {
    clear_stream_params(ctx);

    if (body) {
        char *value;
        if ((value = sdp_attr(body, "rsaaeskey"))) {
            ctx->rtsp.has_key = decrypt_aes_key(ctx, value, ctx->rtsp.aeskey);
            if (!ctx->rtsp.has_key) {
                ESP_LOGE(TAG, "cannot decrypt AES key");
            }
            free(value);
        }
        if ((value = sdp_attr(body, "aesiv"))) {
            ctx->rtsp.has_iv = base64_decode(value, ctx->rtsp.aesiv, sizeof(ctx->rtsp.aesiv)) == 16;
            free(value);
        }
        ctx->rtsp.fmtp = sdp_attr(body, "fmtp");
    }

    remote_start(ctx, kd_lookup(headers, "DACP-ID"), kd_lookup(headers, "Active-Remote"));
}

static bool handle_setup(raop_ctx_t *ctx, const key_data_t *headers, key_data_t *resp) {
    const char *transport = kd_lookup(headers, "Transport");
    const char *p;
    unsigned int cport = 0, tport = 0;

    if (!transport) {
        return false;
    }
    if ((p = strcasestr(transport, "control_port="))) {
        sscanf(p + 13, "%u", &cport);
    }
    if ((p = strcasestr(transport, "timing_port="))) {
        sscanf(p + 12, "%u", &tport);
    }
    if (!cport || !tport) {
        ESP_LOGW(TAG, "SETUP without control/timing ports");
        return false;
    }

    if (!ctx->session_active) {
        raop_event_t event = {.type = RAOP_EVENT_SESSION_START};
        if (!ctx->event_cb(&event)) {
            return false;
        }
        ctx->session_active = true;
    }

    if (ctx->rtp) {
        rtp_end(ctx->rtp);
    }
    bool encrypted = ctx->rtsp.has_key && ctx->rtsp.has_iv;
    rtp_resp_t rtp = rtp_init(ctx->peer, encrypted ? ctx->rtsp.aeskey : NULL, encrypted ? ctx->rtsp.aesiv : NULL,
                              ctx->rtsp.fmtp, cport, tport, ctx->event_cb, ctx->pcm_cb);
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
    } else if (ctype && !strcasecmp(ctype, "image/jpeg")) {
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

    if ((value = kd_lookup(headers, "Apple-Challenge"))) {
        char *response = challenge_response(ctx, sock, value);
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
        parse_rtp_info(headers, &seqno, &rtptime);
        if (ctx->rtp) {
            rtp_record(ctx->rtp, seqno, rtptime);
        }
    } else if (!strcmp(method, "FLUSH")) {
        uint16_t seqno;
        uint32_t rtptime;
        parse_rtp_info(headers, &seqno, &rtptime);
        if (ctx->rtp) {
            rtp_flush(ctx->rtp, seqno, rtptime);
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

raop_ctx_t *raop_create(const char *name, const uint8_t mac[6], raop_event_cb_t event_cb, raop_pcm_cb_t pcm_cb) {
    raop_ctx_t *ctx = calloc(1, sizeof(raop_ctx_t));
    if (!ctx) {
        return NULL;
    }
    ctx->sock = -1;
    memcpy(ctx->mac, mac, 6);
    ctx->event_cb = event_cb;
    ctx->pcm_cb = pcm_cb;
    ctx->remote.done = xSemaphoreCreateBinary();
    ctx->remote.lock = xSemaphoreCreateMutex();

    mbedtls_pk_init(&ctx->pk);
    if (mbedtls_pk_parse_key(&ctx->pk, (const uint8_t *)airport_rsa_key, sizeof(airport_rsa_key), NULL, 0, rng,
                             NULL) != 0) {
        ESP_LOGE(TAG, "cannot parse RSA key");
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

    // Advertised capabilities: ALAC only (no raw PCM decoder here) at
    // 44.1k/16/stereo over UDP, RSA-encrypted or clear, text metadata and
    // JPEG artwork (no progress).
    const mdns_txt_item_t txt[] = {
        {"txtvers", "1"}, {"am", "AudioKnob"}, {"tp", "UDP"},  {"sm", "false"}, {"sv", "false"},
        {"ek", "1"},      {"et", "0,1"},       {"md", "0,1"},    {"cn", "1"},   {"ch", "2"},
        {"ss", "16"},     {"sr", "44100"},     {"vn", "3"},
    };
    char instance[96];
    instance_name(ctx, name, instance, sizeof(instance));
    if (mdns_service_add(instance, "_raop", "_tcp", RTSP_PORT, (mdns_txt_item_t *)txt, sizeof(txt) / sizeof(txt[0])) !=
        ESP_OK) {
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
    mbedtls_pk_free(&ctx->pk);
    vSemaphoreDelete(ctx->remote.done);
    vSemaphoreDelete(ctx->remote.lock);
    free(ctx);
    return NULL;
}

void raop_abort(raop_ctx_t *ctx) {
    ctx->abort = true;
}

bool raop_remote_command(raop_ctx_t *ctx, const char *command) {
    struct sockaddr_in addr = {.sin_family = AF_INET};
    char active_remote[32];

    xSemaphoreTake(ctx->remote.lock, portMAX_DELAY);
    addr.sin_addr = ctx->remote.host;
    addr.sin_port = htons(ctx->remote.port);
    memcpy(active_remote, ctx->remote.active_remote, sizeof(active_remote));
    bool known = ctx->remote.port != 0;
    xSemaphoreGive(ctx->remote.lock);

    if (!known) {
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }
    struct timeval timeout = {REMOTE_TIMEOUT_S, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    bool ok = false;
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        char start_line[128];
        key_data_t headers[3] = {{0}};
        snprintf(start_line, sizeof(start_line), "GET /ctrl-int/1/%s HTTP/1.0", command);
        kd_add(headers, "Active-Remote", active_remote);
        kd_add(headers, "Connection", "close");

        if (http_send(sock, start_line, headers)) {
            char reply[64] = "";
            int n = recv(sock, reply, sizeof(reply) - 1, 0);
            reply[n > 0 ? n : 0] = '\0';
            reply[strcspn(reply, "\r\n")] = '\0';
            ESP_LOGI(TAG, "DACP %s -> %s", command, reply);
            ok = n > 0;
        }
        kd_free(headers);
    } else {
        ESP_LOGW(TAG, "cannot reach DACP remote for %s", command);
    }

    close(sock);
    return ok;
}

bool raop_remote_volume(raop_ctx_t *ctx, float volume) {
    char command[64];
    float db = volume > 0.0f ? (volume - 1.0f) * 30.0f : -144.0f;
    snprintf(command, sizeof(command), "setproperty?dmcp.device-volume=%0.4f", db);
    return raop_remote_command(ctx, command);
}

void raop_set_name(raop_ctx_t *ctx, const char *name) {
    char instance[96];
    instance_name(ctx, name, instance, sizeof(instance));
    if (mdns_service_instance_name_set("_raop", "_tcp", instance) != ESP_OK) {
        ESP_LOGW(TAG, "cannot rename mDNS service");
        return;
    }
    ESP_LOGI(TAG, "AirPlay receiver renamed \"%s\"", name);
}
