// LAN Talk shares RAOP's RTSP/HTTP parser and response writer. This is a
// separate cleartext service; it never enters AirPlay pairing or sessions.
#include "talk_rtsp.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "device_name.h"
#include "http_util.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/idf_additions.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "raop_setup.h"
#include "wifi.h"

#define MAX_HEADERS 16
static talk_rtsp_ops_t s_ops;
static SemaphoreHandle_t s_connections, s_receiver;
static const char *TAG = "talk_rtsp";
static void timeout(int fd) {
    struct timeval tv = {.tv_sec = 1};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}
// A message's reply comes only after the receiver has saved it to flash:
// ~650 ms for text, longer for a voice note.
static void reply_timeout(int fd, int seconds) {
    struct timeval tv = {.tv_sec = seconds};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
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
static bool exchange(int fd, const char *method, const char *path, int cseq, const char *session, const char *transport,
                     const char *type, const void *body, size_t body_size, const char *rtp_info, char *new_session,
                     size_t session_len, uint16_t *server_port) {
    char line[256], seq[16], length[16], name[33];
    device_name_get(name, sizeof(name));
    snprintf(line, sizeof(line), "%s %s RTSP/1.0", method, path);
    snprintf(seq, sizeof(seq), "%d", cseq);
    snprintf(length, sizeof(length), "%u", (unsigned)body_size);
    key_data_t request[] = {{"CSeq", seq},
                            {"Content-Length", length},
                            {"X-AudioKnob-Name", name},
                            {session ? "Session" : "User-Agent", session ? (char *)session : "AudioKnob Talk/1"},
                            {transport ? "Transport" : "Accept", transport ? (char *)transport : "application/sdp"},
                            {type ? "Content-Type" : "Cache-Control", type ? (char *)type : "no-cache"},
                            {rtp_info ? "RTP-Info" : "Pragma", rtp_info ? (char *)rtp_info : "no-cache"},
                            {NULL, NULL}};
    if (!http_send(fd, line, request) || !send_body(fd, body, body_size))
        return false;
    key_data_t response[MAX_HEADERS + 1] = {0};
    char *reply = NULL;
    int reply_len;
    bool ok = http_parse_message(fd, line, sizeof(line), response, MAX_HEADERS, &reply, &reply_len);
    int code = 0;
    if (ok)
        ok = sscanf(line, "RTSP/1.0 %d", &code) == 1 && code == 200;
    const char *value = kd_lookup(response, "CSeq");
    if (ok)
        ok = value && !strcmp(value, seq);
    if (ok && new_session) {
        value = kd_lookup(response, "Session");
        ok = value && strlen(value) < session_len;
        if (ok)
            snprintf(new_session, session_len, "%s", value);
    }
    if (ok && server_port) {
        value = kd_lookup(response, "Transport");
        const char *port = value ? strstr(value, "server_port=") : NULL;
        long n = port ? strtol(port + 12, NULL, 10) : 0;
        ok = n > 0 && n <= 65535;
        if (ok)
            *server_port = n;
    }
    kd_free(response);
    free(reply);
    return ok;
}
int talk_rtsp_begin(uint32_t ip, uint16_t sequence, uint32_t timestamp, uint16_t *peer_port) {
    int fd = connect_peer(ip);
    if (fd < 0)
        return -1;
    char address[16], ssid[33], sdp[512], transport[128], session[40], info[80];
    if (!wifi_get_station(ssid, sizeof(ssid), address, sizeof(address))) {
        close(fd);
        return -1;
    }
    // RFC 7587 uses a 48 kHz RTP clock and /2 even for mono Opus.
    snprintf(sdp, sizeof(sdp),
             "v=0\r\no=- %lu 1 IN IP4 %s\r\ns=AudioKnob Talk\r\nc=IN IP4 %s\r\nt=0 0\r\nm=audio %d RTP/AVP "
             "96\r\na=rtpmap:96 opus/48000/2\r\na=fmtp:96 "
             "stereo=0;sprop-stereo=0;maxplaybackrate=16000;sprop-maxcapturerate=16000\r\na=ptime:20\r\na=sendonly\r\n",
             (unsigned long)esp_random(), address, address, TALK_RTP_PORT);
    snprintf(transport, sizeof(transport), "RTP/AVP/UDP;unicast;client_port=%d;mode=record", TALK_RTP_PORT);
    snprintf(info, sizeof(info), "seq=%u;rtptime=%lu", sequence, (unsigned long)timestamp);
    // Initial sequence belongs in RECORD's RTP-Info (shared RAOP parser).
    if (!exchange(fd, "ANNOUNCE", "/talk", 1, NULL, NULL, "application/sdp", sdp, strlen(sdp), NULL, NULL, 0, NULL) ||
        !exchange(fd, "SETUP", "/talk", 2, NULL, transport, NULL, NULL, 0, NULL, session, sizeof(session), peer_port) ||
        !exchange(fd, "RECORD", "/talk", 3, session, NULL, NULL, NULL, 0, info, NULL, 0, NULL)) {
        close(fd);
        return -1;
    }
    return fd;
}
void talk_rtsp_end(int fd) {
    // Closing TCP is also an unconditional teardown; never wait on a peer
    // while releasing a held finger or yielding the DAC.
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}
bool talk_rtsp_alive(int fd) {
    char byte;
    int n = recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
    return n > 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
}
bool talk_rtsp_message(uint32_t ip, const char *path, const char *json) {
    int fd = connect_peer(ip);
    if (fd < 0)
        return false;
    reply_timeout(fd, 5);
    bool ok =
        exchange(fd, "SET_PARAMETER", path, 1, NULL, NULL, "application/json", json, strlen(json), NULL, NULL, 0, NULL);
    close(fd);
    return ok;
}
bool talk_rtsp_message_bytes(uint32_t ip, const char *path, const char *type, const void *data, size_t length) {
    int fd = connect_peer(ip);
    if (fd < 0)
        return false;
    reply_timeout(fd, 20);
    bool ok = exchange(fd, "SET_PARAMETER", path, 1, NULL, NULL, type, data, length, NULL, NULL, 0, NULL);
    close(fd);
    return ok;
}
static bool reply(int fd, const key_data_t *request, int code, const char *session, uint16_t port) {
    char line[80], transport[128];
    const char *seq = kd_lookup(request, "CSeq");
    snprintf(line, sizeof(line), "RTSP/1.0 %d %s", code,
             code == 200   ? "OK"
             : code == 453 ? "Not Enough Bandwidth"
                           : "Bad Request");
    snprintf(transport, sizeof(transport), "RTP/AVP/UDP;unicast;server_port=%u;mode=record", port);
    key_data_t headers[] = {
        {"CSeq", (char *)(seq ? seq : "0")},
        {"Content-Length", "0"},
        {session ? "Session" : "Server", session ? (char *)session : "AudioKnob Talk/1"},
        {port ? "Transport" : "Public", port ? transport : "ANNOUNCE, SETUP, RECORD, TEARDOWN, SET_PARAMETER, OPTIONS"},
        {NULL, NULL}};
    return http_send(fd, line, headers);
}
static void serve(int fd, uint32_t ip) {
    timeout(fd);
    char session[40], name[33] = "AudioKnob";
    snprintf(session, sizeof(session), "%08lx", (unsigned long)esp_random());
    uint16_t client_port = 0;
    bool announced = false, setup = false, recording = false;
    for (int idle = 0; idle < 30;) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (poll(&p, 1, 100) <= 0) {
            if (recording) {
                if (!s_ops.active())
                    break;
            } else
                idle++;
            continue;
        }
        char line[256], method[32] = "", path[128] = "", *body = NULL;
        int len;
        key_data_t headers[MAX_HEADERS + 1] = {0};
        if (!http_parse_message(fd, line, sizeof(line), headers, MAX_HEADERS, &body, &len))
            break;
        int code = 200;
        bool done = false;
        if (sscanf(line, "%31s %127s RTSP/1.0", method, path) != 2)
            code = 400;
        const char *value = kd_lookup(headers, "X-AudioKnob-Name");
        if (value)
            snprintf(name, sizeof(name), "%s", value);
        if (code == 200 && !strcmp(method, "OPTIONS")) {
        } else if (code == 200 && !strcmp(method, "ANNOUNCE")) {
            const char *type = kd_lookup(headers, "Content-Type");
            announced = type && !strcmp(type, "application/sdp") && body && len <= 2048 &&
                        strstr(body, "a=rtpmap:96 opus/48000/2") && strstr(body, "a=ptime:20");
            if (!announced)
                code = 400;
        } else if (code == 200 && !strcmp(method, "SETUP")) {
            value = kd_lookup(headers, "Transport");
            const char *port = value ? strstr(value, "client_port=") : NULL;
            long n = port ? strtol(port + 12, NULL, 10) : 0;
            if (!announced || !value || !strstr(value, "RTP/AVP/UDP") || n < 1 || n > 65535)
                code = 400;
            else {
                client_port = n;
                setup = true;
            }
        } else if (code == 200 && !strcmp(method, "RECORD")) {
            value = kd_lookup(headers, "Session");
            // Accept regular RAOP-style RTP-Info; our client supplies it too.
            const char *rtp = kd_lookup(headers, "RTP-Info");
            uint16_t first = 0;
            uint32_t time = 0;
            if (rtp)
                raop_parse_rtp_info(rtp, &first, &time);
            if (!setup || recording || !value || strcmp(value, session))
                code = 400;
            else if (xSemaphoreTake(s_receiver, 0) != pdTRUE)
                code = 453;
            else if (!s_ops.start(ip, client_port, first, name)) {
                code = 453;
                xSemaphoreGive(s_receiver);
            } else
                recording = true;
        } else if (code == 200 && !strcmp(method, "TEARDOWN")) {
            value = kd_lookup(headers, "Session");
            if (!value || strcmp(value, session))
                code = 400;
            else
                done = true;
        } else if (code == 200 && !strcmp(method, "SET_PARAMETER")) {
            const char *type = kd_lookup(headers, "Content-Type");
            if (!body || !type || !s_ops.message || !s_ops.message(ip, name, path, type, body, len))
                code = 400;
            done = true;
        } else
            code = 400;
        bool ok = reply(fd, headers, code, setup ? session : NULL,
                        !strcmp(method, "SETUP") && code == 200 ? TALK_RTP_PORT : 0);
        kd_free(headers);
        free(body);
        if (!ok || done || code != 200)
            break;
        idle = 0;
    }
    if (recording) {
        s_ops.stop();
        xSemaphoreGive(s_receiver);
    }
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
    vTaskDelete(NULL);
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
        if (xTaskCreate(connection_task, "talk_conn", 6144, job, 4, NULL) != pdPASS) {
            free(job);
            close(conn);
            xSemaphoreGive(s_connections);
        }
    }
}
void talk_rtsp_init(const talk_rtsp_ops_t *ops) {
    s_ops = *ops;
    s_connections = xSemaphoreCreateCounting(2, 2);
    s_receiver = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_connections && s_receiver ? ESP_OK : ESP_ERR_NO_MEM);
    BaseType_t ok =
        xTaskCreateWithCaps(listener, "talk_rtsp", 3072, NULL, 4, NULL, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
