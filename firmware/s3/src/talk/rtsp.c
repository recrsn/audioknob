// LAN Talk calls share RAOP's RTSP/HTTP parser and response writer. This is
// a separate cleartext service; it never enters AirPlay pairing or sessions.
//
// A call is one TCP connection: ANNOUNCE (SDP), SETUP (each side's RTP
// port), then RECORD, which the callee answers only once its user accepts
// (200 with its own RTP-Info), declines (603), lets it ring out (480) or is
// busy (453). The call lasts as long as the connection.
//
// A voice note is a connection of its own: one SET_PARAMETER /note carrying
// the note (X-Note-Id, X-Note-Wave, Content-Type audio/ogg), answered once
// it's saved; SET_PARAMETER /note/played tells its sender it was played.
// Every request names its knob in X-AudioKnob-Id.
#include "talk/rtsp.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "core/device_name.h"
#include "airplay/shared/http_util.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "airplay/shared/raop_setup.h"
#include "net/wifi/wifi.h"

#define MAX_HEADERS 16
#define REPLY_MS 1000
#define POLL_MS 100
#define RECORD_CSEQ 3
#define USER_AGENT "AudioKnob Talk/3"
// A note's receiver replies once the note is on its flash.
#define NOTE_REPLY_MS 20000
#define CONNECTIONS 3 // a call and two notes
static talk_rtsp_ops_t s_ops;
static char s_key[UI_TALK_KEY];
static SemaphoreHandle_t s_connections, s_receiver;
static const char *TAG = "talk_rtsp";
static void timeout(int fd) {
    struct timeval tv = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
static bool readable(int fd, int ms) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    return poll(&p, 1, ms) > 0;
}
static bool send_body(int fd, const void *data, size_t remaining) {
    const char *body = data;
    while (remaining) {
        int n = send(fd, body, remaining, 0);
        if (n <= 0)
            return false;
        remaining -= n;
        body += n;
    }
    return true;
}
static int connect_peer(uint32_t ip) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons(TALK_RTSP_PORT), .sin_addr.s_addr = ip};
    int rc = connect(fd, (struct sockaddr *)&peer, sizeof(peer));
    if (rc < 0 && errno == EINPROGRESS) {
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        int error = 0;
        socklen_t len = sizeof(error);
        rc = poll(&p, 1, 800) > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && !error ? 0 : -1;
    }
    if (rc < 0) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    timeout(fd);
    return fd;
}
static bool request(int fd, const char *method, int cseq, const char *session, const char *transport,
                    const char *sdp, const char *rtp_info) {
    char line[64], seq[16], length[16], name[33];
    device_name_get(name, sizeof(name));
    snprintf(line, sizeof(line), "%s /talk RTSP/1.0", method);
    snprintf(seq, sizeof(seq), "%d", cseq);
    snprintf(length, sizeof(length), "%u", (unsigned)(sdp ? strlen(sdp) : 0));
    key_data_t headers[10] = {{"CSeq", seq}, {"Content-Length", length}, {"X-AudioKnob-Name", name},
                              {"User-Agent", USER_AGENT}, {"X-AudioKnob-Id", s_key}};
    int n = 5;
    if (session)
        headers[n++] = (key_data_t){"Session", (char *)session};
    if (transport)
        headers[n++] = (key_data_t){"Transport", (char *)transport};
    if (sdp)
        headers[n++] = (key_data_t){"Content-Type", "application/sdp"};
    if (rtp_info)
        headers[n++] = (key_data_t){"RTP-Info", (char *)rtp_info};
    return http_send(fd, line, headers) && (!sdp || send_body(fd, sdp, strlen(sdp)));
}
// The reply's status code, or -1. A 200 must also carry whichever of
// Session, Transport's server_port and RTP-Info's seq are asked for.
static int reply_code(int fd, int cseq, int wait_ms, char *session, size_t session_len, uint16_t *server_port,
                      uint16_t *sequence) {
    char line[256];
    key_data_t response[MAX_HEADERS + 1] = {0};
    char *body = NULL;
    int body_len, code = -1;
    // http_parse_message() gives up on a line after 100 ms, so wait for the
    // reply to start first.
    if (!readable(fd, wait_ms) || !http_parse_message(fd, line, sizeof(line), response, MAX_HEADERS, &body, &body_len))
        goto done;
    const char *value = kd_lookup(response, "CSeq");
    if (sscanf(line, "RTSP/1.0 %d", &code) != 1 || !value || atoi(value) != cseq) {
        code = -1;
        goto done;
    }
    if (code != 200)
        goto done;
    if (session) {
        value = kd_lookup(response, "Session");
        if (!value || strlen(value) >= session_len) {
            code = -1;
            goto done;
        }
        snprintf(session, session_len, "%s", value);
    }
    if (server_port) {
        value = kd_lookup(response, "Transport");
        const char *port = value ? strstr(value, "server_port=") : NULL;
        long n = port ? strtol(port + 12, NULL, 10) : 0;
        if (n <= 0 || n > 65535) {
            code = -1;
            goto done;
        }
        *server_port = n;
    }
    if (sequence) {
        value = kd_lookup(response, "RTP-Info");
        uint32_t time = 0;
        if (!value || !strstr(value, "seq=")) {
            code = -1;
            goto done;
        }
        raop_parse_rtp_info(value, sequence, &time);
    }
done:
    kd_free(response);
    free(body);
    return code;
}
int talk_rtsp_dial(uint32_t ip, uint16_t sequence, uint32_t timestamp, uint16_t *peer_port) {
    int fd = connect_peer(ip);
    if (fd < 0)
        return -1;
    char address[16], ssid[33], sdp[512], transport[96], session[40], info[48];
    if (!wifi_get_station(ssid, sizeof(ssid), address, sizeof(address))) {
        close(fd);
        return -1;
    }
    snprintf(sdp, sizeof(sdp),
             "v=0\r\no=- %lu 1 IN IP4 %s\r\ns=AudioKnob Talk\r\nc=IN IP4 %s\r\nt=0 0\r\nm=audio %d RTP/AVP "
             "96\r\na=rtpmap:96 opus/48000/2\r\na=fmtp:96 "
             "stereo=0;sprop-stereo=0;maxplaybackrate=16000;sprop-maxcapturerate=16000\r\na=ptime:20\r\na=sendrecv\r\n",
             (unsigned long)esp_random(), address, address, TALK_RTP_PORT);
    snprintf(transport, sizeof(transport), "RTP/AVP/UDP;unicast;client_port=%d", TALK_RTP_PORT);
    snprintf(info, sizeof(info), "seq=%u;rtptime=%lu", sequence, (unsigned long)timestamp);
    bool ok = request(fd, "ANNOUNCE", 1, NULL, NULL, sdp, NULL) &&
              reply_code(fd, 1, REPLY_MS, NULL, 0, NULL, NULL) == 200 &&
              request(fd, "SETUP", 2, NULL, transport, NULL, NULL) &&
              reply_code(fd, 2, REPLY_MS, session, sizeof(session), peer_port, NULL) == 200 &&
              request(fd, "RECORD", RECORD_CSEQ, session, NULL, NULL, info);
    if (!ok) {
        close(fd);
        return -1;
    }
    return fd;
}
talk_dial_t talk_rtsp_answer(int fd, uint16_t *sequence) {
    if (!readable(fd, 0))
        return TALK_DIAL_RINGING;
    switch (reply_code(fd, RECORD_CSEQ, 0, NULL, 0, NULL, sequence)) {
    case 200:
        return TALK_DIAL_ANSWERED;
    case 453:
        return TALK_DIAL_BUSY;
    case 480:
        return TALK_DIAL_NO_ANSWER;
    case 603:
        return TALK_DIAL_DECLINED;
    default:
        return TALK_DIAL_FAILED;
    }
}
void talk_rtsp_end(int fd) {
    // Closing TCP is also an unconditional teardown; never wait on a peer
    // while hanging up or yielding the DAC.
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}
static int note_post(uint32_t ip, const char *path, const char *id, const uint8_t *wave, const uint8_t *ogg,
                     size_t len) {
    int fd = connect_peer(ip);
    if (fd < 0)
        return -1;
    char line[64], length[16], name[33], wave_hex[UI_NOTE_WAVE * 2 + 1] = "";
    device_name_get(name, sizeof(name));
    snprintf(line, sizeof(line), "SET_PARAMETER %s RTSP/1.0", path);
    snprintf(length, sizeof(length), "%u", (unsigned)len);
    for (int i = 0; wave && i < UI_NOTE_WAVE; i++)
        snprintf(wave_hex + i * 2, 3, "%02x", wave[i]);
    key_data_t headers[10] = {{"CSeq", "1"},         {"Content-Length", length},  {"X-AudioKnob-Name", name},
                              {"User-Agent", USER_AGENT}, {"X-AudioKnob-Id", s_key}, {"X-Note-Id", (char *)id}};
    int n = 6;
    if (ogg) {
        headers[n++] = (key_data_t){"Content-Type", "audio/ogg"};
        headers[n++] = (key_data_t){"X-Note-Wave", wave_hex};
    }
    int code = http_send(fd, line, headers) && (!ogg || send_body(fd, ogg, len))
                   ? reply_code(fd, 1, NOTE_REPLY_MS, NULL, 0, NULL, NULL)
                   : -1;
    close(fd);
    return code;
}

int talk_rtsp_send_note(uint32_t ip, const char *id, const uint8_t wave[UI_NOTE_WAVE], const uint8_t *ogg, size_t len) {
    return note_post(ip, "/note", id, wave, ogg, len);
}

int talk_rtsp_send_played(uint32_t ip, const char *id) {
    return note_post(ip, "/note/played", id, NULL, NULL, 0);
}

bool talk_rtsp_alive(int fd) {
    char byte;
    int n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return n > 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
}
static bool reply(int fd, const char *cseq, int code, const char *session, uint16_t port, const char *rtp_info) {
    char line[80], transport[96];
    snprintf(line, sizeof(line), "RTSP/1.0 %d %s", code,
             code == 200   ? "OK"
             : code == 453 ? "Not Enough Bandwidth"
             : code == 480 ? "Temporarily Unavailable"
             : code == 503 ? "Service Unavailable"
             : code == 507 ? "Insufficient Storage"
             : code == 603 ? "Decline"
                           : "Bad Request");
    snprintf(transport, sizeof(transport), "RTP/AVP/UDP;unicast;server_port=%u", port);
    key_data_t headers[7] = {{"CSeq", (char *)(cseq ? cseq : "0")}, {"Content-Length", "0"}, {"Server", USER_AGENT}};
    int n = 3;
    if (session)
        headers[n++] = (key_data_t){"Session", (char *)session};
    if (port)
        headers[n++] = (key_data_t){"Transport", transport};
    if (rtp_info)
        headers[n++] = (key_data_t){"RTP-Info", (char *)rtp_info};
    if (!session && !port && !rtp_info)
        headers[n++] = (key_data_t){"Public", "ANNOUNCE, SETUP, RECORD, TEARDOWN, OPTIONS"};
    return http_send(fd, line, headers);
}
typedef struct {
    char session[16], name[33], key[UI_TALK_KEY], cseq[16];
    uint16_t client_port, first;
} offer_t;

static bool parse_wave(const char *hex, uint8_t wave[UI_NOTE_WAVE]) {
    if (!hex || strlen(hex) != UI_NOTE_WAVE * 2) {
        return false;
    }
    for (int i = 0; i < UI_NOTE_WAVE; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1 || v > 100) {
            return false;
        }
        wave[i] = v;
    }
    return true;
}

// SET_PARAMETER: a note, or its played receipt. Takes *body for a note.
static int note_request(const char *path, key_data_t *headers, char **body, int len, const offer_t *o) {
    const char *id = kd_lookup(headers, "X-Note-Id");
    if (!o->key[0] || !id) {
        return 400;
    }
    if (!strcmp(path, "/note/played")) {
        s_ops.played(o->key, id);
        return 200;
    }
    const char *type = kd_lookup(headers, "Content-Type");
    uint8_t wave[UI_NOTE_WAVE];
    if (strcmp(path, "/note") || !type || strcmp(type, "audio/ogg") || !*body ||
        !parse_wave(kd_lookup(headers, "X-Note-Wave"), wave)) {
        return 400;
    }
    uint8_t *ogg = (uint8_t *)*body;
    *body = NULL;
    return s_ops.note(o->key, o->name, id, wave, ogg, len);
}
// ANNOUNCE and SETUP, up to a valid RECORD. False if the caller gave up or
// sent something else first.
static bool negotiate(int fd, offer_t *o) {
    bool announced = false, setup = false;
    for (int idle = 0; idle < 30;) {
        if (!readable(fd, POLL_MS)) {
            idle++;
            continue;
        }
        char line[256], method[32] = "", path[128] = "", *body = NULL;
        int len;
        key_data_t headers[MAX_HEADERS + 1] = {0};
        if (!http_parse_message(fd, line, sizeof(line), headers, MAX_HEADERS, &body, &len))
            return false;
        int code = sscanf(line, "%31s %127s RTSP/1.0", method, path) == 2 ? 200 : 400;
        const char *value = kd_lookup(headers, "X-AudioKnob-Name");
        if (value)
            snprintf(o->name, sizeof(o->name), "%s", value);
        value = kd_lookup(headers, "X-AudioKnob-Id");
        if (value && strlen(value) == UI_TALK_KEY - 1)
            snprintf(o->key, sizeof(o->key), "%s", value);
        bool record = false, note = false;
        if (code != 200 || !strcmp(method, "OPTIONS")) {
        } else if (!strcmp(method, "SET_PARAMETER")) {
            code = note_request(path, headers, &body, len, o);
            note = true;
        } else if (!strcmp(method, "ANNOUNCE")) {
            const char *type = kd_lookup(headers, "Content-Type");
            announced = type && !strcmp(type, "application/sdp") && body && len <= 2048 &&
                        strstr(body, "a=rtpmap:96 opus/48000/2") && strstr(body, "a=ptime:20") &&
                        strstr(body, "a=sendrecv");
            if (!announced)
                code = 400;
        } else if (!strcmp(method, "SETUP")) {
            value = kd_lookup(headers, "Transport");
            const char *port = value ? strstr(value, "client_port=") : NULL;
            long n = port ? strtol(port + 12, NULL, 10) : 0;
            if (!announced || !strstr(value ? value : "", "RTP/AVP/UDP") || n < 1 || n > 65535)
                code = 400;
            else {
                o->client_port = n;
                setup = true;
            }
        } else if (!strcmp(method, "RECORD")) {
            value = kd_lookup(headers, "Session");
            const char *rtp = kd_lookup(headers, "RTP-Info");
            uint32_t time = 0;
            if (!setup || !value || strcmp(value, o->session) || !rtp || !strstr(rtp, "seq="))
                code = 400;
            else {
                raop_parse_rtp_info(rtp, &o->first, &time);
                value = kd_lookup(headers, "CSeq");
                snprintf(o->cseq, sizeof(o->cseq), "%s", value ? value : "0");
                record = true;
            }
        } else
            code = 400;
        bool ok = record || reply(fd, kd_lookup(headers, "CSeq"), code, setup ? o->session : NULL,
                                  !strcmp(method, "SETUP") && code == 200 ? TALK_RTP_PORT : 0, NULL);
        kd_free(headers);
        free(body);
        if (record)
            return true;
        if (!ok || code != 200 || note)
            return false;
        idle = 0;
    }
    return false;
}
// Rings until the user answers, the caller hangs up (closing the connection
// or TEARDOWN; nothing else is sent while RECORD waits) or it rings out.
// Returns RECORD's reply code, or 0 when the caller hung up.
static int ring(int fd, uint32_t call, uint16_t *sequence, uint32_t *timestamp) {
    for (int waited = 0; waited < TALK_RING_MS; waited += POLL_MS) {
        talk_answer_t answer = s_ops.answer(call, sequence, timestamp);
        if (answer == TALK_ANSWER_ACCEPTED)
            return 200;
        if (answer == TALK_ANSWER_DECLINED)
            return 603;
        if (readable(fd, POLL_MS))
            return 0;
    }
    return 480;
}
// The call: until either side hangs up.
static void hold(int fd, uint32_t call) {
    while (s_ops.active(call)) {
        if (!readable(fd, POLL_MS))
            continue;
        char line[256];
        key_data_t headers[MAX_HEADERS + 1] = {0};
        char *body = NULL;
        int len;
        if (!http_parse_message(fd, line, sizeof(line), headers, MAX_HEADERS, &body, &len)) {
            kd_free(headers);
            return;
        }
        bool teardown = !strncmp(line, "TEARDOWN ", 9);
        bool ok = reply(fd, kd_lookup(headers, "CSeq"), teardown || !strncmp(line, "OPTIONS ", 8) ? 200 : 400, NULL,
                        0, NULL);
        kd_free(headers);
        free(body);
        if (!ok || teardown)
            return;
    }
}
static void serve(int fd, uint32_t ip) {
    timeout(fd);
    offer_t o = {.name = "AudioKnob"};
    snprintf(o.session, sizeof(o.session), "%08lx", (unsigned long)esp_random());
    if (!negotiate(fd, &o)) {
        close(fd);
        return;
    }
    if (xSemaphoreTake(s_receiver, 0) != pdTRUE) {
        reply(fd, o.cseq, 453, o.session, 0, NULL);
        close(fd);
        return;
    }
    uint32_t call = s_ops.ring(ip, o.client_port, o.first, o.key, o.name);
    if (!call) {
        reply(fd, o.cseq, 453, o.session, 0, NULL);
    } else {
        uint16_t sequence = 0;
        uint32_t timestamp = 0;
        int code = ring(fd, call, &sequence, &timestamp);
        char info[48];
        snprintf(info, sizeof(info), "seq=%u;rtptime=%lu", sequence, (unsigned long)timestamp);
        if (code && reply(fd, o.cseq, code, o.session, 0, code == 200 ? info : NULL) && code == 200)
            hold(fd, call);
        s_ops.stop(call);
    }
    xSemaphoreGive(s_receiver);
    close(fd);
}
typedef struct {
    int fd;
    uint32_t ip;
} connection_t;
static void connection_task(void *arg) {
    connection_t conn = *(connection_t *)arg;
    free(arg);
    serve(conn.fd, conn.ip);
    xSemaphoreGive(s_connections);
    vTaskDeleteWithCaps(NULL);
}
static void listener(void *arg) {
    (void)arg;
    uint16_t port = TALK_RTSP_PORT;
    int fd = bind_socket(&port, SOCK_STREAM);
    if (fd < 0 || listen(fd, 2) < 0) {
        if (fd >= 0)
            close(fd);
        ESP_LOGE(TAG, "listener failed");
        vTaskDeleteWithCaps(NULL);
        return;
    }
    for (;;) {
        struct sockaddr_in peer;
        socklen_t len = sizeof(peer);
        int conn = accept(fd, (struct sockaddr *)&peer, &len);
        if (conn < 0)
            continue;
        connection_t *job = malloc(sizeof(*job));
        if (!job || xSemaphoreTake(s_connections, 0) != pdTRUE) {
            free(job);
            close(conn);
            continue;
        }
        *job = (connection_t){conn, peer.sin_addr.s_addr};
        // RTSP control only, never flash: a PSRAM stack.
        if (xTaskCreateWithCaps(connection_task, "talk_conn", 6144, job, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) !=
            pdPASS) {
            free(job);
            close(conn);
            xSemaphoreGive(s_connections);
        }
    }
}
void talk_rtsp_init(const talk_rtsp_ops_t *ops, const char *key) {
    s_ops = *ops;
    snprintf(s_key, sizeof(s_key), "%s", key);
    s_connections = xSemaphoreCreateCounting(CONNECTIONS, CONNECTIONS);
    s_receiver = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_connections && s_receiver ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t ok =
        xTaskCreateWithCaps(listener, "talk_rtsp", 3072, NULL, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
