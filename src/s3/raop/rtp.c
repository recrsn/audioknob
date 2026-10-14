/*
 * HairTunes - RAOP packet handler and slave-clocked replay engine
 * Copyright (c) James Laird 2011
 * All rights reserved.
 *
 * Modularisation: philippe_44@outlook.com, 2019
 * ESP-IDF 5 / AudioKnob port: per-session buffers in PSRAM, typed events.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "rtp.h"

#include <stdlib.h>
#include <string.h>

#include "alac_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "http_util.h"
#include "lwip/sockets.h"
#include "mbedtls/aes.h"

// Sender clock offset: keep the last few NTP exchanges and trust the one
// with the shortest network delay, whose midpoint estimate is the tightest.
#define TIMING_HISTORY 8
#define MAX_ROUNDTRIP_US 100000

// Power of two that divides 2^16, so BUFIDX() stays collision-free across
// the 16-bit seqno wrap (~4 s of 352-frame packets).
#define BUFFER_FRAMES 512
#define BUFIDX(seqno) ((seq_t)(seqno) % BUFFER_FRAMES)
#define MAX_PACKET 1408
// ALAC's bit reader peeks past the end of the payload.
#define PACKET_SLACK 16
// Every AirPlay 1 sender uses 352-frame packets; the player is fixed at it too.
#define MAX_FRAME_SIZE 352
#define MIN_LATENCY 11025
#define MAX_LATENCY ((120 * RAOP_SAMPLE_RATE * 2) / 100)
#define RESEND_TO_MS 250
#define SELECT_TIMEOUT_MS 100
#define STALL_TICKS (30 * 1000 / SELECT_TIMEOUT_MS)

#define RTP_STACK_SIZE 6144
#define RTP_TASK_PRIO 8
// Off core 0, where the WiFi and lwIP tasks that feed it run.
#define RTP_TASK_CORE 1

#define RTP_SYNC 0x01
#define NTP_SYNC 0x02

enum { DATA = 0, CONTROL, TIMING };

typedef uint16_t seq_t;

typedef struct {
    uint32_t rtptime, last_resend;
    int16_t *data;
    uint16_t frames;
    bool ready, missed;
} abuf_t;

struct rtp_s {
    volatile bool running;
    uint8_t aesiv[16];
    mbedtls_aes_context aes;
    bool decrypt;
    uint8_t *decrypt_buf;
    uint8_t *packet;
    uint32_t frame_size;
    struct in_addr host;
    struct sockaddr_in rtp_host;
    struct {
        uint16_t rport, lport;
        int sock;
    } sockets[3];
    struct {
        int64_t offset_us;  // sender NTP clock minus esp_timer clock
        int64_t offsets[TIMING_HISTORY], delays[TIMING_HISTORY];
        int count, next;
    } timing;
    struct {
        uint32_t rtp;
        int64_t remote_us;  // sender time at which rtp plays
        uint8_t status;
    } synchro;
    int latency;  // rtp hold depth in samples
    uint32_t resent_req, resent_rec, silent_frames, discarded, in_frames, out_frames;
    abuf_t *audio_buffer;
    seq_t ab_read, ab_write;
    SemaphoreHandle_t ab_mutex;
    TaskHandle_t thread, joiner;
    alac_dec_t *alac;
    int first_seqno;
    enum { RTP_WAIT, RTP_STREAM, RTP_PLAY } state;
    int stalled;
    raop_event_cb_t event_cb;
    raop_pcm_cb_t pcm_cb;
};

static const char *TAG = "rtp";

static void rtp_thread_func(void *arg);
static bool rtp_request_resend(rtp_t *ctx, seq_t first, seq_t last);
static bool rtp_request_timing(rtp_t *ctx);

static uint16_t read_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void write_be16(uint8_t *p, uint16_t v) {
    p[0] = v >> 8;
    p[1] = v;
}

static void write_be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

// Sequence numbers wrap often; true if b is after a.
static bool seq_order(seq_t a, seq_t b) {
    int16_t d = b - a;
    return d > 0;
}

static void emit(rtp_t *ctx, raop_event_type_t type) {
    raop_event_t event = {.type = type};
    ctx->event_cb(&event);
}

static bool parse_fmtp(const char *fmtp, int out[12]) {
    char *copy = strdup(fmtp ? fmtp : "");
    char *cursor = copy, *arg;
    int n = 0;

    memset(out, 0, 12 * sizeof(int));
    while (copy && n < 12 && (arg = strsep(&cursor, " \t")) != NULL) {
        if (*arg) {
            out[n++] = atoi(arg);
        }
    }
    free(copy);
    return n == 12;
}

static const int16_t s_silence[MAX_FRAME_SIZE * 2];

// Allocated on the first session and kept: re-allocating ~720 KB of
// contiguous PSRAM per SETUP can fail once the heap fragments. Only one RTP
// session exists at a time (raop ends the old one before starting the next).
static abuf_t *s_audio_buffer;  // internal: touched on every packet
static int16_t *s_pcm_pool;

static bool buffers_get(rtp_t *ctx) {
    if (!s_audio_buffer) {
        s_audio_buffer = malloc(BUFFER_FRAMES * sizeof(abuf_t));
    }
    if (!s_pcm_pool) {
        s_pcm_pool = heap_caps_malloc((size_t)BUFFER_FRAMES * MAX_FRAME_SIZE * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    }
    if (!s_audio_buffer || !s_pcm_pool) {
        return false;
    }
    for (int i = 0; i < BUFFER_FRAMES; i++) {
        s_audio_buffer[i] = (abuf_t){.data = s_pcm_pool + (size_t)i * MAX_FRAME_SIZE * 2};
    }
    ctx->audio_buffer = s_audio_buffer;
    return true;
}

static void buffer_reset(rtp_t *ctx) {
    for (int i = 0; i < BUFFER_FRAMES; i++) {
        ctx->audio_buffer[i].ready = false;
    }
}

rtp_resp_t rtp_init(struct in_addr host, const uint8_t *aeskey, const uint8_t *aesiv, const char *fmtp,
                    uint16_t ctrl_port, uint16_t timing_port, raop_event_cb_t event_cb, raop_pcm_cb_t pcm_cb) {
    rtp_resp_t resp = {0};
    rtp_t *ctx = calloc(1, sizeof(rtp_t));
    if (!ctx) {
        return resp;
    }

    ctx->host = host;
    ctx->event_cb = event_cb;
    ctx->pcm_cb = pcm_cb;
    ctx->rtp_host.sin_family = AF_INET;
    ctx->rtp_host.sin_addr.s_addr = INADDR_ANY;
    ctx->ab_mutex = xSemaphoreCreateMutex();
    ctx->first_seqno = -1;
    for (int i = 0; i < 3; i++) {
        ctx->sockets[i].sock = -1;
    }
    ctx->sockets[CONTROL].rport = ctrl_port;
    ctx->sockets[TIMING].rport = timing_port;

    ctx->packet = malloc(MAX_PACKET + PACKET_SLACK);
    bool ok = ctx->ab_mutex && ctx->packet;

    if (aeskey && aesiv) {
        memcpy(ctx->aesiv, aesiv, sizeof(ctx->aesiv));
        mbedtls_aes_init(&ctx->aes);
        mbedtls_aes_setkey_dec(&ctx->aes, aeskey, 128);
        ctx->decrypt = true;
        ctx->decrypt_buf = malloc(MAX_PACKET + PACKET_SLACK);
        ok &= ctx->decrypt_buf != NULL;
    }

    int fmtp_vals[12];
    if (!parse_fmtp(fmtp, fmtp_vals) || fmtp_vals[1] <= 0 || fmtp_vals[1] > MAX_FRAME_SIZE) {
        ESP_LOGE(TAG, "unsupported fmtp: %s", fmtp ? fmtp : "(none)");
        ok = false;
    } else {
        ctx->frame_size = fmtp_vals[1];
        ctx->alac = alac_dec_create(fmtp_vals);
        ok &= ctx->alac != NULL;
        if (!ctx->alac) {
            ESP_LOGE(TAG, "ALAC decoder create failed");
        }
        if (!buffers_get(ctx)) {
            ESP_LOGE(TAG, "audio buffer alloc failed");
            ok = false;
        }
    }

    for (int i = 0; ok && i < 3; i++) {
        ctx->sockets[i].sock = bind_socket(&ctx->sockets[i].lport, SOCK_DGRAM);
        ok &= ctx->sockets[i].sock >= 0;
        if (ctx->sockets[i].sock < 0) {
            ESP_LOGE(TAG, "bind socket %d failed: errno %d", i, errno);
        }
    }

    if (ok) {
        ctx->running = true;
        ok = xTaskCreatePinnedToCore(rtp_thread_func, "rtp", RTP_STACK_SIZE, ctx, RTP_TASK_PRIO, &ctx->thread,
                                     RTP_TASK_CORE) == pdPASS;
        ctx->running = ok;
        if (!ok) {
            ESP_LOGE(TAG, "rtp task create failed, free internal %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }
    }

    if (!ok) {
        ESP_LOGE(TAG, "cannot start RTP");
        rtp_end(ctx);
        return resp;
    }
    // Internal RAM headroom is what decides whether the next SETUP's task create succeeds.
    ESP_LOGI(TAG, "started; internal free %u, largest %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    resp.cport = ctx->sockets[CONTROL].lport;
    resp.tport = ctx->sockets[TIMING].lport;
    resp.aport = ctx->sockets[DATA].lport;
    resp.ctx = ctx;
    return resp;
}

void rtp_end(rtp_t *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->running) {
        ctx->joiner = xTaskGetCurrentTaskHandle();
        ctx->running = false;
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    for (int i = 0; i < 3; i++) {
        if (ctx->sockets[i].sock >= 0) {
            close(ctx->sockets[i].sock);
        }
    }
    if (ctx->alac) {
        alac_dec_delete(ctx->alac);
    }
    if (ctx->decrypt) {
        mbedtls_aes_free(&ctx->aes);
    }
    if (ctx->ab_mutex) {
        vSemaphoreDelete(ctx->ab_mutex);
    }
    free(ctx->decrypt_buf);
    free(ctx->packet);
    free(ctx);
}

void rtp_flush(rtp_t *ctx, uint16_t seqno, uint32_t rtptime) {
    xSemaphoreTake(ctx->ab_mutex, portMAX_DELAY);

    // Always store the flush seqno: only packets strictly above it are wanted,
    // even when it equals the one from RECORD.
    ctx->first_seqno = seqno;
    if (ctx->state == RTP_PLAY) {
        buffer_reset(ctx);
        ctx->state = RTP_WAIT;
        ESP_LOGI(TAG, "FLUSH packets below %u - %lu", seqno, (unsigned long)rtptime);
        emit(ctx, RAOP_EVENT_FLUSH);
    }

    xSemaphoreGive(ctx->ab_mutex);
}

void rtp_record(rtp_t *ctx, uint16_t seqno, uint32_t rtptime) {
    xSemaphoreTake(ctx->ab_mutex, portMAX_DELAY);
    ctx->first_seqno = (seqno || rtptime) ? seqno : -1;
    ctx->state = RTP_WAIT;
    xSemaphoreGive(ctx->ab_mutex);
    ESP_LOGI(TAG, "record %u - %lu", seqno, (unsigned long)rtptime);
}

static bool decode_packet(rtp_t *ctx, uint8_t *buf, int len, abuf_t *abuf) {
    uint8_t *in = buf;

    if (ctx->decrypt) {
        // AES-128-CBC over whole blocks, IV reset every packet; the trailing
        // partial block is sent in the clear.
        uint8_t iv[16];
        int aeslen = len & ~0xf;
        memcpy(iv, ctx->aesiv, sizeof(iv));
        mbedtls_aes_crypt_cbc(&ctx->aes, MBEDTLS_AES_DECRYPT, aeslen, iv, buf, ctx->decrypt_buf);
        memcpy(ctx->decrypt_buf + aeslen, buf + aeslen, len - aeslen);
        in = ctx->decrypt_buf;
    }

    uint32_t frames = 0;
    if (!alac_dec_decode(ctx->alac, in, len, abuf->data, &frames) || frames == 0 || frames > ctx->frame_size) {
        ESP_LOGW(TAG, "ALAC decode failed (%d bytes)", len);
        return false;
    }
    abuf->frames = frames;
    return true;
}

static int64_t ntp_to_us(uint64_t ntp) {
    return (int64_t)(ntp >> 32) * 1000000 + (int64_t)(((ntp & 0xffffffff) * 1000000) >> 32);
}

static uint64_t us_to_ntp(int64_t us) {
    return ((uint64_t)(us / 1000000) << 32) | (((uint64_t)(us % 1000000) << 32) / 1000000);
}

// Local (esp_timer) time at which rtptime should reach the DAC. Applies the
// latest clock offset, so a better NTP estimate takes effect immediately.
static int64_t playtime_of(rtp_t *ctx, uint32_t rtptime) {
    int32_t delta = (int32_t)(rtptime - ctx->synchro.rtp);
    return ctx->synchro.remote_us - ctx->timing.offset_us + (int64_t)delta * 1000000 / RAOP_SAMPLE_RATE;
}

// Pushes as many frames as possible downstream, in order.
static void buffer_push_packet(rtp_t *ctx) {
    int64_t hold = ((int64_t)ctx->latency * 1000000) / (8 * RAOP_SAMPLE_RATE);
    if (hold < 100000) {
        hold = 100000;
    }

    if (ctx->state != RTP_PLAY || ctx->synchro.status != (RTP_SYNC | NTP_SYNC)) {
        return;
    }

    int64_t now, playtime;
    do {
        // Re-read the clock each loop: the downstream callback may take time.
        now = esp_timer_get_time();
        abuf_t *frame = ctx->audio_buffer + BUFIDX(ctx->ab_read);
        playtime = playtime_of(ctx, frame->rtptime);

        if (now > playtime) {
            ESP_LOGD(TAG, "discarded frame, missed by %ld ms (W:%u R:%u)", (long)((now - playtime) / 1000),
                     ctx->ab_write, ctx->ab_read);
            ctx->discarded++;
            frame->ready = false;
        } else if (playtime - now <= hold) {
            // Out of time to wait for a resend: play what we have, or silence.
            if (frame->ready) {
                ctx->pcm_cb(frame->data, frame->frames, playtime);
                frame->ready = false;
            } else {
                ctx->pcm_cb(s_silence, ctx->frame_size, playtime);
                ctx->silent_frames++;
                frame->missed = true;
            }
        } else if (frame->ready) {
            ctx->pcm_cb(frame->data, frame->frames, playtime);
            frame->ready = false;
        } else {
            break;
        }

        ctx->ab_read++;
        ctx->out_frames++;
    } while (seq_order(ctx->ab_read, ctx->ab_write));

    if (ctx->out_frames > 1000) {
        ESP_LOGI(TAG, "drain [level:%d head:%ld ms] [W:%u R:%u] [req:%lu sil:%lu dis:%lu]",
                 (int16_t)(ctx->ab_write - ctx->ab_read), (long)((playtime - now) / 1000), ctx->ab_write, ctx->ab_read,
                 (unsigned long)ctx->resent_req, (unsigned long)ctx->silent_frames, (unsigned long)ctx->discarded);
        ctx->out_frames = 0;
    }

    // Request resends of missing packets in order. Every frame is checked:
    // sampling would fold ready frames into the requested ranges, and the
    // sender answers each duplicate, which snowballs under packet loss.
    int first = -1;
    for (int i = 0; seq_order(ctx->ab_read + i, ctx->ab_write); i++) {
        abuf_t *frame = ctx->audio_buffer + BUFIDX(ctx->ab_read + i);

        // Stop the run at a ready frame or one with a recent pending resend.
        if (first >= 0 && (frame->ready || now - frame->last_resend <= RESEND_TO_MS)) {
            if (!rtp_request_resend(ctx, first, ctx->ab_read + i - 1)) {
                break;
            }
            first = -1;
        } else if (!frame->ready && now - frame->last_resend > RESEND_TO_MS) {
            if (first < 0) {
                first = (seq_t)(ctx->ab_read + i);
            }
            frame->last_resend = now;
        }
    }
}

static void buffer_put_packet(rtp_t *ctx, seq_t seqno, uint32_t rtptime, uint8_t *data, int len) {
    xSemaphoreTake(ctx->ab_mutex, portMAX_DELAY);

    /* After a RECORD with a seqno we are in RTP_WAIT and that seqno is the
     * first allowed one. If RECORD's seqno was 0, a FLUSH will tell us where
     * to start: accept everything (RTP_STREAM) and clean up once it arrives.
     * After a FLUSH while playing we are in RTP_WAIT with a known seqno and
     * must not accept anything before it. */
    if (ctx->first_seqno != -1 && seq_order(seqno, ctx->first_seqno)) {
        xSemaphoreGive(ctx->ab_mutex);
        return;
    }

    if (ctx->state == RTP_WAIT) {
        ctx->ab_write = seqno - 1;
        ctx->ab_read = ctx->ab_write + 1;
        ctx->resent_req = ctx->resent_rec = ctx->silent_frames = ctx->discarded = 0;
        if (ctx->first_seqno != -1) {
            ESP_LOGI(TAG, "1st accepted packet:%u, now playing", seqno);
            ctx->state = RTP_PLAY;
            ctx->first_seqno = -1;
            emit(ctx, RAOP_EVENT_PLAY);
        } else {
            ctx->state = RTP_STREAM;
            ESP_LOGI(TAG, "1st accepted packet:%u, waiting for FLUSH", seqno);
        }
    } else if (ctx->state == RTP_STREAM && ctx->first_seqno != -1 && seq_order(ctx->first_seqno, seqno + 1)) {
        // Discard everything below first_seqno and anything not yet ready.
        while (seq_order(ctx->ab_read, ctx->first_seqno) || !ctx->audio_buffer[BUFIDX(ctx->ab_read)].ready) {
            ctx->audio_buffer[BUFIDX(ctx->ab_read)].ready = false;
            ctx->ab_read++;
        }
        ESP_LOGI(TAG, "done waiting for FLUSH with packet:%u, now playing from:%u", seqno, ctx->ab_read);
        ctx->state = RTP_PLAY;
        ctx->first_seqno = -1;
        emit(ctx, RAOP_EVENT_PLAY);
    }

    abuf_t *abuf = ctx->audio_buffer + BUFIDX(seqno);

    if (seqno == (seq_t)(ctx->ab_write + 1)) {
        ctx->ab_write = seqno;
    } else if (seq_order(ctx->ab_write, seqno)) {
        seq_t gap = seqno - ctx->ab_write - 1;
        int limit = BUFFER_FRAMES / 2;
        if (ctx->latency && ctx->latency / (int)ctx->frame_size < limit) {
            limit = ctx->latency / ctx->frame_size;
        }

        if (gap > limit) {
            ESP_LOGW(TAG, "too many missing frames %u seq:%u (W:%u R:%u)", gap, seqno, ctx->ab_write, ctx->ab_read);
            ctx->ab_read = seqno;
        } else {
            if (ctx->state == RTP_PLAY) {
                rtp_request_resend(ctx, ctx->ab_write + 1, seqno - 1);
            }
            // Expected timing of the missing frames, for buffer_push_packet.
            uint32_t now = raop_time_ms();
            for (seq_t i = ctx->ab_write + 1; seq_order(i, seqno); i++) {
                ctx->audio_buffer[BUFIDX(i)].rtptime = rtptime - (seq_t)(seqno - i) * ctx->frame_size;
                ctx->audio_buffer[BUFIDX(i)].last_resend = now;
            }
        }
        ctx->ab_write = seqno;
    } else if (seq_order(ctx->ab_read, seqno + 1)) {
        ctx->resent_rec++;
        // A duplicate resend: decoding it again only burns CPU.
        if (abuf->ready) {
            abuf = NULL;
        }
    } else {
        if (abuf->missed) {
            ESP_LOGI(TAG, "packet too late seqno:%u (W:%u R:%u)", seqno, ctx->ab_write, ctx->ab_read);
        }
        abuf = NULL;
    }

    if (ctx->in_frames++ > 1000) {
        ESP_LOGI(TAG, "fill [level:%d rec:%lu] [W:%u R:%u]", (int16_t)(ctx->ab_write - ctx->ab_read),
                 (unsigned long)ctx->resent_rec, ctx->ab_write, ctx->ab_read);
        ctx->in_frames = 0;
    }

    if (abuf) {
        abuf->ready = decode_packet(ctx, data, len, abuf);
        abuf->missed = false;
        abuf->rtptime = rtptime;
        buffer_push_packet(ctx);
    }

    xSemaphoreGive(ctx->ab_mutex);
}

static void handle_sync(rtp_t *ctx, const uint8_t *pkt, int *count) {
    uint32_t rtp_now_latency = read_be32(pkt + 4);
    uint64_t remote = ((uint64_t)read_be32(pkt + 8) << 32) | read_be32(pkt + 12);
    uint32_t rtp_now = read_be32(pkt + 16);
    uint16_t flags = read_be16(pkt + 2);

    // Refresh NTP every 3 sync packets, or every time until synced.
    if (!(*count)-- || !(ctx->synchro.status & NTP_SYNC)) {
        rtp_request_timing(ctx);
        *count = 3;
    }

    xSemaphoreTake(ctx->ab_mutex, portMAX_DELAY);

    // Re-align timestamp and expected local playback time (and the magic
    // 11025-sample latency some senders expect).
    ctx->latency = rtp_now - rtp_now_latency;
    if (flags == 7 || flags == 4) {
        ctx->latency += 11025;
    }
    if (ctx->latency < MIN_LATENCY) {
        ctx->latency = MIN_LATENCY;
    } else if (ctx->latency > MAX_LATENCY) {
        ctx->latency = MAX_LATENCY;
    }
    ctx->synchro.rtp = rtp_now - ctx->latency;
    ctx->synchro.remote_us = ntp_to_us(remote);
    ctx->synchro.status |= RTP_SYNC;

    xSemaphoreGive(ctx->ab_mutex);

    ESP_LOGD(TAG, "sync: latency:%d rtp:%lu", ctx->latency, (unsigned long)rtp_now);
}

static uint64_t read_be64(const uint8_t *p) {
    return ((uint64_t)read_be32(p) << 32) | read_be32(p + 4);
}

static void handle_timing(rtp_t *ctx, const uint8_t *pkt) {
    int64_t arrival = esp_timer_get_time();
    // origin echoes the transmit time of our request, in local time.
    int64_t origin = ntp_to_us(read_be64(pkt + 8));
    int64_t receive = ntp_to_us(read_be64(pkt + 16));
    int64_t transmit = ntp_to_us(read_be64(pkt + 24));
    int64_t delay = (arrival - origin) - (transmit - receive);

    if (delay < 0 || delay > MAX_ROUNDTRIP_US) {
        if (!(ctx->synchro.status & NTP_SYNC)) {
            rtp_request_timing(ctx);
        }
        ESP_LOGW(TAG, "discarding NTP roundtrip of %ld us", (long)delay);
        return;
    }

    int i = ctx->timing.next;
    ctx->timing.offsets[i] = ((receive - origin) + (transmit - arrival)) / 2;
    ctx->timing.delays[i] = delay;
    ctx->timing.next = (i + 1) % TIMING_HISTORY;
    if (ctx->timing.count < TIMING_HISTORY) {
        ctx->timing.count++;
    }

    int best = i;
    for (int j = 0; j < ctx->timing.count; j++) {
        if (ctx->timing.delays[j] < ctx->timing.delays[best]) {
            best = j;
        }
    }

    xSemaphoreTake(ctx->ab_mutex, portMAX_DELAY);
    ctx->timing.offset_us = ctx->timing.offsets[best];
    ctx->synchro.status |= NTP_SYNC;
    xSemaphoreGive(ctx->ab_mutex);

    ESP_LOGD(TAG, "timing: delay:%ld us offset:%lld us (best delay:%ld us)", (long)delay,
             (long long)ctx->timing.offset_us, (long)ctx->timing.delays[best]);
}

static void rtp_thread_func(void *arg) {
    rtp_t *ctx = arg;
    uint8_t *packet = ctx->packet;
    int max_sock = -1;
    int count = 0;
    bool ntp_sent = false;

    for (int i = 0; i < 3; i++) {
        if (ctx->sockets[i].sock > max_sock) {
            max_sock = ctx->sockets[i].sock;
        }
        ntp_sent = rtp_request_timing(ctx);
    }

    while (ctx->running) {
        fd_set fds;
        struct timeval timeout = {0, SELECT_TIMEOUT_MS * 1000};

        FD_ZERO(&fds);
        for (int i = 0; i < 3; i++) {
            FD_SET(ctx->sockets[i].sock, &fds);
        }

        if (select(max_sock + 1, &fds, NULL, NULL, &timeout) <= 0) {
            // Senders go quiet while paused (after FLUSH), so only silence
            // during playback means the sender is gone.
            if (ctx->state != RTP_PLAY) {
                ctx->stalled = 0;
            } else if (ctx->stalled++ == STALL_TICKS) {
                emit(ctx, RAOP_EVENT_STALLED);
            }
            continue;
        }

        int idx = 0;
        for (int i = 0; i < 3; i++) {
            if (FD_ISSET(ctx->sockets[i].sock, &fds)) {
                idx = i;
            }
        }

        socklen_t addr_len = sizeof(ctx->rtp_host);
        int plen = recvfrom(ctx->sockets[idx].sock, packet, MAX_PACKET, MSG_DONTWAIT, (struct sockaddr *)&ctx->rtp_host,
                            &addr_len);

        if (!ntp_sent) {
            ntp_sent = rtp_request_timing(ctx);
        }
        if (plen <= 0) {
            continue;
        }
        ctx->stalled = 0;

        uint8_t *pkt = packet;
        uint8_t type = packet[1] & ~0x80;

        switch (type) {
        case 0x56:  // resent packet: data packet behind a 4-byte header
            pkt += 4;
            plen -= 4;
            // fall through
        case 0x60: {
            if (plen < 12 + 16) {
                break;
            }
            seq_t seqno = read_be16(pkt + 2);
            uint32_t rtptime = read_be32(pkt + 4);
            buffer_put_packet(ctx, seqno, rtptime, pkt + 12, plen - 12);
            break;
        }
        case 0x54:
            if (plen >= 20) {
                handle_sync(ctx, pkt, &count);
            }
            break;
        case 0x53:
            if (plen >= 24) {
                handle_timing(ctx, pkt);
            }
            break;
        default:
            ESP_LOGW(TAG, "unknown packet type %x", type);
            break;
        }
    }

    ESP_LOGI(TAG, "terminating");
    xTaskNotifyGive(ctx->joiner);
    vTaskDelete(NULL);
}

static bool rtp_request_timing(rtp_t *ctx) {
    uint8_t req[32] = {0};
    struct sockaddr_in host = {.sin_family = AF_INET};

    req[0] = 0x80;
    req[1] = 0x52 | 0x80;
    write_be16(req + 2, 7);
    // The sender echoes "transmit" back as "origin" in its reply.
    uint64_t now = us_to_ntp(esp_timer_get_time());
    write_be32(req + 24, now >> 32);
    write_be32(req + 28, now);

    host.sin_addr = ctx->host.s_addr != INADDR_ANY ? ctx->host : ctx->rtp_host.sin_addr;
    // No sender address yet: wait for its first packet.
    if (host.sin_addr.s_addr == INADDR_ANY) {
        return false;
    }
    host.sin_port = htons(ctx->sockets[TIMING].rport);

    if (sendto(ctx->sockets[TIMING].sock, req, sizeof(req), MSG_DONTWAIT, (struct sockaddr *)&host, sizeof(host)) !=
        sizeof(req)) {
        ESP_LOGW(TAG, "timing request failed: %s", strerror(errno));
    }
    return true;
}

static bool rtp_request_resend(rtp_t *ctx, seq_t first, seq_t last) {
    uint8_t req[8];  // Apple's resend request, not a standard RTCP NACK

    // Avoid absurd ranges (happens after long network blackouts).
    if (seq_order(last, first) || (seq_t)(last - first) > BUFFER_FRAMES / 2) {
        return false;
    }
    ctx->resent_req += (seq_t)(last - first) + 1;

    req[0] = 0x80;
    req[1] = 0x55 | 0x80;
    write_be16(req + 2, 1);
    write_be16(req + 4, first);
    write_be16(req + 6, last - first + 1);

    ctx->rtp_host.sin_port = htons(ctx->sockets[CONTROL].rport);
    if (sendto(ctx->sockets[CONTROL].sock, req, sizeof(req), MSG_DONTWAIT, (struct sockaddr *)&ctx->rtp_host,
               sizeof(ctx->rtp_host)) != sizeof(req)) {
        ESP_LOGW(TAG, "resend request failed: %s", strerror(errno));
        return false;
    }
    return true;
}
