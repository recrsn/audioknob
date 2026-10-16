/*
 * Event channel, after shairport-sync's ap2_event_receiver.c
 * (Copyright (c) Mike Brady 2014--2026, MIT License).
 */

#ifndef AK_NATIVE

#include "event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ap2.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "rtsp.h"

#define TASK_STACK 4096
#define TASK_PRIO 5
#define ACCEPT_POLL_MS 500
#define REPLY_TIMEOUT_MS 2000
#define REPLY_MAX 4096

struct event_channel {
    const ap2_conn_t *conn;
    int listen_fd;
    int fd;
    volatile bool stop;
    cipher_stream_t cipher;
    SemaphoreHandle_t lock;  // one request/reply exchange at a time
    SemaphoreHandle_t done;  // given when the accept task exits
};

static const char *TAG = "ap2_event";

// Reads one RTSP reply and returns its status code, or -1.
static int read_reply_into(event_channel_t *ev, char *buf) {
    size_t len = 0;
    char *end = NULL;
    while (!end) {
        if (len == REPLY_MAX) {
            return -1;
        }
        ssize_t n = cipher_stream_read(ev->fd, &ev->cipher, buf + len, REPLY_MAX - len, NULL);
        if (n <= 0) {
            return -1;
        }
        len += (size_t)n;
        buf[len] = '\0';
        end = strstr(buf, "\r\n\r\n");
    }
    int code;
    if (sscanf(buf, "RTSP/1.%*d %d", &code) != 1) {
        return -1;
    }
    // Drain any body so the next exchange starts on a reply boundary.
    const char *cl = strcasestr(buf, "\r\nContent-Length:");
    size_t body = cl ? strtoul(cl + 17, NULL, 10) : 0;
    size_t have = len - (size_t)(end + 4 - buf);
    while (have < body) {
        size_t want = body - have < REPLY_MAX ? body - have : REPLY_MAX;
        ssize_t n = cipher_stream_read(ev->fd, &ev->cipher, buf, want, NULL);
        if (n <= 0) {
            return -1;
        }
        have += (size_t)n;
    }
    return code;
}

// Callers run on small stacks (the UI's control task among them).
static int read_reply(event_channel_t *ev) {
    char *buf = ap2_malloc(REPLY_MAX + 1);
    if (!buf) {
        return -1;
    }
    int code = read_reply_into(ev, buf);
    ap2_free(buf);
    return code;
}

static bool post_locked(event_channel_t *ev, const uint8_t *plist, size_t plist_len) {
    char head[160];
    int n = snprintf(head, sizeof(head),
                     "POST /command RTSP/1.0\r\nContent-Length: %u\r\n"
                     "Content-Type: application/x-apple-binary-plist\r\n\r\n",
                     (unsigned)plist_len);
    uint8_t *msg = malloc((size_t)n + plist_len);
    if (!msg) {
        return false;
    }
    memcpy(msg, head, (size_t)n);
    memcpy(msg + n, plist, plist_len);
    bool ok = cipher_stream_write(ev->fd, &ev->cipher, msg, (size_t)n + plist_len);
    free(msg);
    if (!ok) {
        return false;
    }
    int code = read_reply(ev);
    if (code < 200 || code >= 300) {
        ESP_LOGW(TAG, "sender replied %d", code);
        return false;
    }
    return true;
}

bool event_post_command(event_channel_t *ev, bpw_t *w, bpw_obj root) {
    size_t len;
    uint8_t *plist = bpw_serialize(w, root, &len);
    if (!plist) {
        return false;
    }
    bool ok = false;
    xSemaphoreTake(ev->lock, portMAX_DELAY);
    if (ev->fd >= 0) {
        ok = post_locked(ev, plist, len);
    }
    xSemaphoreGive(ev->lock);
    free(plist);
    return ok;
}

static void send_update_info(event_channel_t *ev) {
    bpw_t *w = bpw_new();
    if (!w) {
        return;
    }
    bpw_obj d = bpw_dict(w);
    bpw_dict_set(w, d, "type", bpw_string(w, "updateInfo"));
    bpw_dict_set(w, d, "value", rtsp_info_plist(w, ev->conn));
    if (!event_post_command(ev, w, d)) {
        ESP_LOGD(TAG, "updateInfo not acknowledged");
    }
    bpw_free(w);
}

static void accept_task(void *arg) {
    event_channel_t *ev = arg;
    while (!ev->stop) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(ev->listen_fd, &set);
        struct timeval tv = {.tv_usec = ACCEPT_POLL_MS * 1000};
        if (select(ev->listen_fd + 1, &set, NULL, NULL, &tv) <= 0) {
            continue;
        }
        int fd = accept(ev->listen_fd, NULL, NULL);
        if (fd < 0) {
            continue;
        }
        struct timeval rto = {.tv_sec = REPLY_TIMEOUT_MS / 1000, .tv_usec = (REPLY_TIMEOUT_MS % 1000) * 1000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));
        xSemaphoreTake(ev->lock, portMAX_DELAY);
        ev->fd = fd;
        xSemaphoreGive(ev->lock);
        ESP_LOGI(TAG, "sender connected");
        send_update_info(ev);
        break;
    }
    xSemaphoreGive(ev->done);
    ap2_task_exit();
}

event_channel_t *event_open(const ap2_conn_t *conn, uint16_t *port) {
    event_channel_t *ev = calloc(1, sizeof(*ev));
    if (!ev) {
        return NULL;
    }
    ev->conn = conn;
    ev->fd = -1;
    ev->lock = xSemaphoreCreateMutex();
    ev->done = xSemaphoreCreateBinary();
    ev->cipher.ctx = pair_cipher_new(PAIR_SERVER_HOMEKIT, PAIR_CHANNEL_EVENTS, conn->shared_secret,
                                     conn->shared_secret_len, "");
    ev->listen_fd = ap2_listen_any(SOCK_STREAM, port);
    if (!ev->lock || !ev->done || !ev->cipher.ctx || ev->listen_fd < 0 ||
        !ap2_task_start(accept_task, "ap2_event", TASK_STACK, ev, TASK_PRIO, 0, true)) {
        if (ev->listen_fd >= 0) {
            close(ev->listen_fd);
        }
        cipher_stream_free(&ev->cipher);
        if (ev->lock) {
            vSemaphoreDelete(ev->lock);
        }
        if (ev->done) {
            vSemaphoreDelete(ev->done);
        }
        free(ev);
        return NULL;
    }
    return ev;
}

void event_close(event_channel_t *ev) {
    ev->stop = true;
    xSemaphoreTake(ev->done, portMAX_DELAY);
    close(ev->listen_fd);
    xSemaphoreTake(ev->lock, portMAX_DELAY);
    if (ev->fd >= 0) {
        close(ev->fd);
    }
    xSemaphoreGive(ev->lock);
    cipher_stream_free(&ev->cipher);
    vSemaphoreDelete(ev->lock);
    vSemaphoreDelete(ev->done);
    free(ev);
}

#endif  // !AK_NATIVE
