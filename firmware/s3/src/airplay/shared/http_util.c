/*
 * RTSP/HTTP helpers for the RAOP receiver.
 *
 * Ported from philippe44's AirConnect / squeezelite-esp32 util.c
 * (c) Philippe 2016-2017, philippe_44@outlook.com -- MIT License,
 * https://opensource.org/licenses/MIT
 */

#include "airplay/shared/http_util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lwip/sockets.h"

#define LINE_TIMEOUT_MS 100
// Artwork JPEGs are the largest bodies a sender sends; anything bigger is
// drained rather than letting a peer pick the allocation size.
#define MAX_BODY_LEN (1024 * 1024)

static const char *TAG = "http_util";

// Returns the line length, 0 on timeout/peer close, -1 on socket error.
static int read_line(int sock, char *line, int maxlen) {
    struct pollfd pfd = {.fd = sock, .events = POLLIN};
    int count = 0;

    while (count < maxlen - 1) {
        if (poll(&pfd, 1, LINE_TIMEOUT_MS) <= 0) {
            break;
        }

        char ch;
        int n = recv(sock, &ch, 1, 0);
        if (n < 0) {
            if (errno == EAGAIN) {
                break;
            }
            ESP_LOGE(TAG, "socket %d read error: %s", sock, strerror(errno));
            line[count] = '\0';
            return -1;
        }
        if (n == 0) {
            break;
        }
        if (ch == '\n') {
            line[count] = '\0';
            return count;
        }
        if (ch != '\r') {
            line[count++] = ch;
        }
    }

    line[count] = '\0';
    return count;
}

static bool recv_exact(int sock, char *buf, int len) {
    int received = 0;
    while (received < len) {
        int n = recv(sock, buf + received, len - received, 0);
        if (n <= 0) {
            ESP_LOGE(TAG, "body receive error %d/%d", received, len);
            return false;
        }
        received += n;
    }
    return true;
}

// Keeps the connection in sync past a body we won't store.
static bool drain(int sock, long len) {
    char scratch[256];
    while (len > 0) {
        int chunk = len < (long)sizeof(scratch) ? (int)len : (int)sizeof(scratch);
        if (!recv_exact(sock, scratch, chunk)) {
            return false;
        }
        len -= chunk;
    }
    return true;
}

static char *ltrim(char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

bool http_parse_message(int sock, char *start_line, size_t start_size, key_data_t *kd, int max_headers, char **body, int *body_len) {
    char line[256];
    int i = 0;
    long content_len = 0;

    kd[0].key = NULL;
    *body = NULL;
    *body_len = 0;

    if (read_line(sock, line, sizeof(line)) <= 0) {
        return false;
    }

    snprintf(start_line, start_size, "%s", line);

    while (read_line(sock, line, sizeof(line)) > 0) {
        // Obsolete line folding: append to the previous header's value.
        if (i && (line[0] == ' ' || line[0] == '\t')) {
            char *cont = ltrim(line);
            char *joined = realloc(kd[i - 1].data, strlen(kd[i - 1].data) + strlen(cont) + 1);
            if (joined) {
                strcat(joined, cont);
                kd[i - 1].data = joined;
            }
            continue;
        }

        char *colon = strchr(line, ':');
        if (!colon) {
            ESP_LOGE(TAG, "bad header line");
            kd_free(kd);
            return false;
        }
        if (i == max_headers) {
            ESP_LOGW(TAG, "dropping header beyond %d: %s", max_headers, line);
            continue;
        }

        *colon = '\0';
        kd[i].key = strdup(line);
        kd[i].data = strdup(ltrim(colon + 1));
        if (!strcasecmp(kd[i].key, "Content-Length")) {
            content_len = strtol(kd[i].data, NULL, 10);
        }
        kd[++i].key = NULL;
    }

    if (content_len > MAX_BODY_LEN) {
        ESP_LOGW(TAG, "dropping %ld-byte body", content_len);
        if (!drain(sock, content_len)) {
            kd_free(kd);
            return false;
        }
    } else if (content_len > 0) {
        // PSRAM: bodies are parsed once, never on a hot path.
        *body = heap_caps_malloc(content_len + 1, MALLOC_CAP_SPIRAM);
        if (!*body || !recv_exact(sock, *body, (int)content_len)) {
            free(*body);
            *body = NULL;
            kd_free(kd);
            return false;
        }
        (*body)[content_len] = '\0';
        *body_len = (int)content_len;
    }

    return true;
}

bool http_send(int sock, const char *start_line, const key_data_t *kd) {
    size_t len = strlen(start_line) + 4 + 1;
    for (int i = 0; kd && kd[i].key; i++) {
        len += strlen(kd[i].key) + 2 + strlen(kd[i].data) + 2;
    }

    char *buf = malloc(len);
    if (!buf) {
        return false;
    }

    size_t pos = snprintf(buf, len, "%s\r\n", start_line);
    for (int i = 0; kd && kd[i].key; i++) {
        pos += snprintf(buf + pos, len - pos, "%s: %s\r\n", kd[i].key, kd[i].data);
    }
    pos += snprintf(buf + pos, len - pos, "\r\n");

    bool ok = send(sock, buf, pos, 0) == (ssize_t)pos;
    if (!ok) {
        ESP_LOGE(TAG, "send failed: %s", strerror(errno));
    }
    free(buf);
    return ok;
}

const char *kd_lookup(const key_data_t *kd, const char *key) {
    for (int i = 0; kd && kd[i].key; i++) {
        if (!strcasecmp(kd[i].key, key)) {
            return kd[i].data;
        }
    }
    return NULL;
}

void kd_add(key_data_t *kd, const char *key, const char *data) {
    int i = 0;
    while (kd[i].key) {
        i++;
    }
    kd[i].key = strdup(key);
    kd[i].data = strdup(data);
    kd[i + 1].key = NULL;
}

void kd_free(key_data_t *kd) {
    for (int i = 0; kd && kd[i].key; i++) {
        free(kd[i].key);
        free(kd[i].data);
    }
    kd[0].key = NULL;
}

int bind_socket(uint16_t *port, int type) {
    int sock = socket(AF_INET, type, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "cannot create socket");
        return -1;
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(*port),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "cannot bind socket: %s", strerror(errno));
        close(sock);
        return -1;
    }

    if (!*port) {
        socklen_t len = sizeof(addr);
        getsockname(sock, (struct sockaddr *)&addr, &len);
        *port = ntohs(addr.sin_port);
    }
    return sock;
}

bool http_parse(int sock, char *method, size_t method_size, key_data_t *kd, int max_headers, char **body, int *body_len) {
    char start[256];
    if (!http_parse_message(sock, start, sizeof(start), kd, max_headers, body, body_len)) return false;
    char fmt[16];
    snprintf(fmt, sizeof(fmt), "%%%us", (unsigned)(method_size - 1));
    if (sscanf(start, fmt, method) == 1) return true;
    kd_free(kd); free(*body); *body = NULL; *body_len = 0;
    return false;
}
