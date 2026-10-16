/*
 * AirPlay 2 RTP control and realtime audio receivers, after shairport-sync's
 * rtp.c (rtp_ap2_control_receiver, rtp_realtime_audio_receiver,
 * decipher_player_put_packet; Copyright (c) James Laird 2013, Mike Brady
 * 2014--2025, MIT License).
 */

#ifndef AK_NATIVE

#include "rtp.h"

#include <string.h>

#include "ap2.h"
#include "decoder.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "session.h"
#include "sodium.h"

#define PACKET_MAX 2048
#define ANCHOR_PACKET 215
#define RESENT_AUDIO_PACKET 0xD6
// Realtime streams state a 77175-frame latency; shairport-sync found this
// many more frames bring playback into sync.
#define LATENCY_FUDGE_FRAMES 11025
// A realtime stream with no anchors this long has lost its timing.
#define ANCHOR_TIMEOUT_MS 7000
#define POLL_MS 100

#define RT_FRAMES 352
#define RESEND_REQUEST 0xD5
// Packets held for reordering, ~0.5 s: a hole waits this long for its resend
// unless its playtime comes first. The sender runs ~2 s ahead.
#define WINDOW 64
#define HOLE_DEADLINE_US 100000
#define LOSS_LOG_EVERY 10

// Measured peaks: control ~1.3 KB, realtime receive (with ALAC decode) ~2.2 KB.
#define CONTROL_STACK 3072
#define AUDIO_STACK 4096
#define CONTROL_PRIO 8
#define AUDIO_PRIO 8
#define TASK_CORE 0

typedef struct {
    bool held;
    uint16_t seq;
    uint16_t frames;
    uint32_t timestamp;
    uint32_t epoch;
    int16_t pcm[RT_FRAMES * 2];
} held_t;

struct rtp {
    uint8_t key[32];
    bool realtime;
    int control_fd, audio_fd;
    volatile bool stop;
    SemaphoreHandle_t audio_lock;  // the decoder and the window, used from both tasks
    decoder_t *dec;
    int16_t *pcm;
    uint8_t *plain;
    SemaphoreHandle_t control_done, audio_done;

    // Reorder window: packets arrive in any order, leave in sequence.
    held_t *window;
    bool started;
    uint16_t next_seq;     // the next packet to hand on
    uint16_t highest_seq;  // the newest packet seen
    bool have_ts;
    uint32_t next_ts;  // the timestamp next_seq should carry
    uint32_t window_epoch;

    // Where the sender's control packets come from; resend requests go there.
    struct sockaddr_in sender_ctrl;
    uint16_t resend_seq;
    uint32_t requested, recovered, lost;
    int64_t min_lead_us;  // how far ahead of its playtime audio was handed on
};

static const int16_t s_silence[RT_FRAMES * 2];

static const char *TAG = "ap2_rtp";

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p) {
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static void reset_window(rtp_t *r, uint16_t seq, uint32_t epoch) {
    for (int i = 0; i < WINDOW; i++) {
        r->window[i].held = false;
    }
    r->started = true;
    r->next_seq = seq;
    r->highest_seq = seq - 1;
    r->have_ts = false;
    r->window_epoch = epoch;
    r->min_lead_us = INT64_MAX;
}

static void request_resend(rtp_t *r, uint16_t first, uint16_t count) {
    if (!r->sender_ctrl.sin_port) {
        return;
    }
    uint16_t id = r->resend_seq++;
    uint8_t req[8] = {0x80, RESEND_REQUEST, id >> 8, id & 0xFF, first >> 8, first & 0xFF, count >> 8, count & 0xFF};
    sendto(r->control_fd, req, sizeof(req), 0, (struct sockaddr *)&r->sender_ctrl, sizeof(r->sender_ctrl));
    r->requested += count;
}

static void deliver(rtp_t *r, const int16_t *pcm, size_t frames, uint32_t timestamp, uint32_t epoch) {
    int64_t playtime_us;
    if (session_playtime_us(timestamp, &playtime_us)) {
        session_pcm(pcm, frames, playtime_us, epoch);
        int64_t lead = playtime_us - esp_timer_get_time();
        r->min_lead_us = lead < r->min_lead_us ? lead : r->min_lead_us;
    }
    r->next_ts = timestamp + (uint32_t)frames;
    r->have_ts = true;
    r->next_seq++;
}

// Hands on next_seq if held, else gives up on it: a lost packet plays as
// silence, so the player's timeline has no hole for its servo to absorb.
static void advance(rtp_t *r) {
    held_t *h = &r->window[r->next_seq % WINDOW];
    if (h->held && h->seq == r->next_seq) {
        h->held = false;
        deliver(r, h->pcm, h->frames, h->timestamp, h->epoch);
        return;
    }
    if (r->lost++ % LOSS_LOG_EVERY == 0) {
        ESP_LOGW(TAG, "packet %u lost (%lu lost, %lu of %lu resends recovered)", r->next_seq,
                 (unsigned long)r->lost, (unsigned long)r->recovered, (unsigned long)r->requested);
    }
    if (r->have_ts) {
        deliver(r, s_silence, RT_FRAMES, r->next_ts, r->window_epoch);
    } else {
        r->next_seq++;
    }
}

// A missing next_seq waits for its resend until its playtime is close.
static bool hole_expired(rtp_t *r) {
    int64_t playtime_us;
    return !r->have_ts || !session_playtime_us(r->next_ts, &playtime_us) ||
           playtime_us - esp_timer_get_time() < HOLE_DEADLINE_US;
}

// Hands on everything held from next_seq in order, and expired holes.
static void release(rtp_t *r) {
    for (;;) {
        held_t *h = &r->window[r->next_seq % WINDOW];
        bool held = h->held && h->seq == r->next_seq;
        // Past the newest packet is the stream's leading edge, not a hole.
        if (!held && ((int16_t)(r->highest_seq - r->next_seq) < 0 || !hole_expired(r))) {
            return;
        }
        advance(r);
    }
}

// p: sequence number (2 bytes), timestamp (4), SSRC (4), ciphertext and
// tag, then the 8-byte nonce.
static void put_packet(rtp_t *r, const uint8_t *p, size_t len) {
    if (len <= 10 + 16 + 8) {
        return;
    }
    uint16_t seq = (uint16_t)(p[0] << 8 | p[1]);
    uint32_t timestamp = be32(p + 2);

    xSemaphoreTake(r->audio_lock, portMAX_DELAY);
    uint32_t epoch = session_epoch();
    // A flush starts a new sequence; a jump this far either way is a new stream too.
    int16_t jump = (int16_t)(seq - r->next_seq);
    if (!r->started || epoch != r->window_epoch || jump >= 2 * WINDOW || jump < -2 * WINDOW) {
        reset_window(r, seq, epoch);
    }
    int16_t ahead = (int16_t)(seq - r->next_seq);
    held_t *h = &r->window[seq % WINDOW];
    // Late resends and duplicates.
    if (ahead < 0 || (h->held && h->seq == seq)) {
        xSemaphoreGive(r->audio_lock);
        return;
    }

    if ((int16_t)(seq - r->highest_seq) > 0) {
        uint16_t missing = seq - r->highest_seq - 1;
        if (missing > 0 && missing < WINDOW) {
            request_resend(r, r->highest_seq + 1, missing);
        }
        r->highest_seq = seq;
    } else {
        r->recovered++;
    }
    // Make room: what this packet's slot still waits for is given up.
    while ((uint16_t)(seq - r->next_seq) >= WINDOW) {
        advance(r);
    }

    uint8_t nonce[12] = {0};
    memcpy(nonce + 4, p + len - 8, 8);
    unsigned long long plain_len = 0;
    int frames = -1;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(r->plain, &plain_len, NULL, p + 10, len - 10 - 8, p + 2, 8, nonce,
                                                  r->key) == 0) {
        frames = decoder_decode(r->dec, r->plain, (size_t)plain_len, r->pcm, DECODER_MAX_FRAMES);
    }
    if (frames > RT_FRAMES) {
        ESP_LOGW(TAG, "packet %u: %d frames, expected %d", seq, frames, RT_FRAMES);
        frames = -1;
    }
    if (frames > 0) {
        if (seq == r->next_seq) {
            deliver(r, r->pcm, (size_t)frames, timestamp, epoch);
        } else {
            *h = (held_t){.held = true, .seq = seq, .frames = (uint16_t)frames, .timestamp = timestamp, .epoch = epoch};
            memcpy(h->pcm, r->pcm, (size_t)frames * 2 * sizeof(int16_t));
        }
    }
    release(r);
    xSemaphoreGive(r->audio_lock);
}

static void handle_anchor(const uint8_t *p, size_t len) {
    if (len < 28) {
        return;
    }
    uint32_t frame_1 = be32(p + 4);  // the frame playing, latency included
    uint64_t remote_ns = be64(p + 8);
    uint32_t frame_2 = be32(p + 16);  // the frame remote_ns refers to
    uint64_t clock_id = be64(p + 20);
    if (frame_2 - frame_1 != 77175) {
        ESP_LOGD(TAG, "stream latency %ld frames", (long)(frame_2 - frame_1));
    }
    session_set_anchor(clock_id, frame_1 - LATENCY_FUDGE_FRAMES, (int64_t)remote_ns);
}

static void control_task(void *arg) {
    rtp_t *r = arg;
    uint8_t *packet = ap2_malloc(PACKET_MAX);
    int quiet_ms = 0;

    while (packet && !r->stop) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(r->control_fd, packet, PACKET_MAX, 0, (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            quiet_ms += POLL_MS;
            if (r->realtime && quiet_ms == ANCHOR_TIMEOUT_MS) {
                ESP_LOGW(TAG, "no control packets for %d s, timing lost", ANCHOR_TIMEOUT_MS / 1000);
                session_clear_anchor();
            }
            continue;
        }
        quiet_ms = 0;
        if (n < 8 || !r->realtime) {
            continue;
        }
        if (from.sin_port != r->sender_ctrl.sin_port || from.sin_addr.s_addr != r->sender_ctrl.sin_addr.s_addr) {
            xSemaphoreTake(r->audio_lock, portMAX_DELAY);
            r->sender_ctrl = from;
            xSemaphoreGive(r->audio_lock);
        }
        switch (packet[1]) {
        case ANCHOR_PACKET:
            handle_anchor(packet, (size_t)n);
            break;
        case RESENT_AUDIO_PACKET:
            put_packet(r, packet + 6, (size_t)n - 6);
            break;
        default:
            ESP_LOGD(TAG, "control packet type 0x%02x, %d bytes", packet[1], n);
            break;
        }
    }
    ap2_free(packet);
    xSemaphoreGive(r->control_done);
    ap2_task_exit();
}

static void audio_task(void *arg) {
    rtp_t *r = arg;
    uint8_t *packet = ap2_malloc(PACKET_MAX);

    while (packet && !r->stop) {
        int n = recv(r->audio_fd, packet, PACKET_MAX, 0);
        // 12-byte RTP header plus 24 bytes of tag and nonce.
        if (n > 36) {
            put_packet(r, packet + 2, (size_t)n - 2);
        } else if (n < 0) {
            // Quiet: a hole at the end of a burst still expires.
            xSemaphoreTake(r->audio_lock, portMAX_DELAY);
            if (r->started) {
                release(r);
            }
            xSemaphoreGive(r->audio_lock);
        }
    }
    ap2_free(packet);
    xSemaphoreGive(r->audio_done);
    ap2_task_exit();
}

static void set_poll_timeout(int fd) {
    struct timeval tv = {.tv_usec = POLL_MS * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static void destroy(rtp_t *r) {
    if (r->control_fd >= 0) {
        close(r->control_fd);
    }
    if (r->audio_fd >= 0) {
        close(r->audio_fd);
    }
    decoder_close(r->dec);
    ap2_free(r->window);
    ap2_free(r->pcm);
    ap2_free(r->plain);
    if (r->audio_lock) {
        vSemaphoreDelete(r->audio_lock);
    }
    if (r->control_done) {
        vSemaphoreDelete(r->control_done);
    }
    if (r->audio_done) {
        vSemaphoreDelete(r->audio_done);
    }
    memset(r->key, 0, sizeof(r->key));
    free(r);
}

rtp_t *rtp_start(const uint8_t key[32], bool realtime, uint16_t *data_port, uint16_t *control_port) {
    rtp_t *r = calloc(1, sizeof(*r));
    if (!r) {
        return NULL;
    }
    memcpy(r->key, key, sizeof(r->key));
    r->realtime = realtime;
    r->audio_fd = -1;
    r->control_fd = ap2_listen_any(SOCK_DGRAM, control_port);
    r->audio_lock = xSemaphoreCreateMutex();
    r->control_done = xSemaphoreCreateBinary();
    r->audio_done = xSemaphoreCreateBinary();
    bool ok = r->control_fd >= 0 && r->audio_lock && r->control_done && r->audio_done;
    if (ok && realtime) {
        r->audio_fd = ap2_listen_any(SOCK_DGRAM, data_port);
        r->dec = decoder_open_alac();
        r->pcm = ap2_malloc(DECODER_MAX_FRAMES * 2 * sizeof(int16_t));
        r->plain = ap2_malloc(PACKET_MAX);
        r->window = ap2_malloc(WINDOW * sizeof(held_t));
        ok = r->audio_fd >= 0 && r->dec && r->pcm && r->plain && r->window;
        if (r->window) {
            memset(r->window, 0, WINDOW * sizeof(held_t));
        }
    }
    if (!ok) {
        destroy(r);
        return NULL;
    }
    set_poll_timeout(r->control_fd);
    if (!ap2_task_start(control_task, "ap2_ctrl", CONTROL_STACK, r, CONTROL_PRIO, TASK_CORE, false)) {
        destroy(r);
        return NULL;
    }
    if (realtime) {
        set_poll_timeout(r->audio_fd);
        if (!ap2_task_start(audio_task, "ap2_rt_rx", AUDIO_STACK, r, AUDIO_PRIO, TASK_CORE, false)) {
            r->stop = true;
            xSemaphoreTake(r->control_done, portMAX_DELAY);
            destroy(r);
            return NULL;
        }
    } else {
        xSemaphoreGive(r->audio_done);
    }
    return r;
}

void rtp_stop(rtp_t *r) {
    r->stop = true;
    xSemaphoreTake(r->control_done, portMAX_DELAY);
    xSemaphoreTake(r->audio_done, portMAX_DELAY);
    if (r->realtime) {
        ESP_LOGI(TAG, "stream done: %lu lost, %lu of %lu resends recovered, min lead %lld ms", (unsigned long)r->lost,
                 (unsigned long)r->recovered, (unsigned long)r->requested,
                 r->min_lead_us == INT64_MAX ? -1LL : (long long)(r->min_lead_us / 1000));
    }
    destroy(r);
}

#endif  // !AK_NATIVE
