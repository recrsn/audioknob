/*
 * RTP control and realtime audio receivers for AirPlay 2 and AirPlay 1,
 * after shairport-sync's rtp.c (rtp_ap2_control_receiver,
 * rtp_realtime_audio_receiver, rtp_control_receiver, rtp_audio_receiver,
 * decipher_player_put_packet) and player.c (player_put_packet's resend
 * requests); Copyright (c) James Laird 2013, Mike Brady 2014--2026, MIT
 * License. Both protocols share the reorder window and resends; they differ
 * in the cipher (ChaCha20-Poly1305 or AES-CBC), the payload (ALAC, or L16
 * for AirPlay 1) and the clock (PTP anchors or AirPlay 1's NTP and sync
 * packets).
 */

#include "airplay/receiver/rtp.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "airplay/receiver/ap1/clock.h"
#include "airplay/receiver/ap2.h"
#include "airplay/shared/decoder.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/aes.h"
#include "airplay/receiver/session.h"
#include "sodium.h"

#define PACKET_MAX 2048
#define ANCHOR_PACKET 0xD7
#define SYNC_PACKET 0xD4
#define TIMING_REPLY 0xD3
#define RESENT_AUDIO_PACKET 0xD6
// Realtime streams state a 77175-frame latency; shairport-sync found this
// many more frames bring playback into sync.
#define LATENCY_FUDGE_FRAMES 11025
// A realtime stream with no anchors this long has lost its timing.
#define ANCHOR_TIMEOUT_US (7 * 1000000LL)
// An AirPlay 1 sender silent this long while playing is gone.
#define AP1_STALL_US (30 * 1000000LL)
#define POLL_MS 100

#define RT_FRAMES 352
#define RESEND_REQUEST 0xD5
// Packets held for reordering, ~0.5 s: a hole waits this long for its resend
// unless its playtime comes first. The sender runs ~2 s ahead.
#define WINDOW 64
#define HOLE_DEADLINE_US 100000
// shairport-sync's resend_control_*: a hole is asked for once it has been
// missing this long (a reordered packet fills it before then), and again
// at this interval while it stays missing and isn't due.
#define RESEND_FIRST_US 100000
#define RESEND_REPEAT_US 250000
#define LOSS_LOG_EVERY 10

// AirPlay 1 timing requests: a few quickly to sync, well within the reorder
// window that holds audio until then, then a slow refresh.
#define TIMING_FAST_US 50000
#define TIMING_SLOW_US 3000000
#define TIMING_FAST_COUNT 4

// Measured peak ~2.5 KB, with ALAC decode.
#define TASK_STACK 4096
#define TASK_PRIO 8
#define TASK_CORE 0

typedef struct {
    bool held;
    uint16_t seq;  // held, or the missing packet noticed_us refers to
    uint16_t frames;
    uint32_t timestamp;
    uint32_t epoch;
    int64_t noticed_us, requested_us;
    int32_t pcm[RT_FRAMES * 2];
} held_t;

struct rtp {
    bool realtime;
    bool ap1;
    int control_fd, audio_fd, timing_fd;
    volatile bool stop;
    SemaphoreHandle_t audio_lock;  // the decoder and the window
    decoder_t *dec;
    int32_t *pcm;
    uint8_t *plain;
    SemaphoreHandle_t done;  // given when the receive task exits
    uint16_t frames_per_packet;

    // AirPlay 2: the session key.
    uint8_t key[32];
    // AirPlay 1: AES-CBC with the IV reset per packet, or clear; ALAC or
    // big-endian L16.
    mbedtls_aes_context aes;
    bool aes_on;
    uint8_t aesiv[16];
    bool l16;
    ap1_clock_t clock;
    struct sockaddr_in sender_timing;
    // Packets before floor_seq predate a FLUSH or RECORD and don't play.
    bool have_floor;
    uint16_t floor_seq;
    int64_t last_audio_us;
    bool stall_reported;

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

static const char *TAG = "ap2_rtp";

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p) {
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static bool playtime_us(rtp_t *r, uint32_t timestamp, int64_t *out) {
    return r->ap1 ? ap1_clock_playtime_us(&r->clock, timestamp, out) : session_playtime_us(timestamp, AP2_RATE, out);
}

// AirPlay 1 audio waits in the window until the sender's clock is measured
// and a sync packet has placed it; AirPlay 2 audio arrives anchored.
static bool timed(rtp_t *r, uint32_t timestamp) {
    int64_t unused;
    return !r->ap1 || playtime_us(r, timestamp, &unused);
}

static void reset_window(rtp_t *r, uint16_t seq, uint32_t epoch) {
    for (int i = 0; i < WINDOW; i++) {
        r->window[i].held = false;
        r->window[i].seq = seq - 1;  // matches no packet in the new window
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

static void deliver(rtp_t *r, const int32_t *pcm, size_t frames, uint32_t timestamp, uint32_t epoch) {
    int64_t at_us;
    if (playtime_us(r, timestamp, &at_us)) {
        session_pcm(pcm, frames, at_us, AP2_RATE, epoch);
        int64_t lead = at_us - esp_timer_get_time();
        r->min_lead_us = lead < r->min_lead_us ? lead : r->min_lead_us;
    }
    r->next_ts = timestamp + (uint32_t)frames;
    r->have_ts = true;
    r->next_seq++;
}

static bool is_held(const rtp_t *r, uint16_t seq) {
    const held_t *h = &r->window[seq % WINDOW];
    return h->held && h->seq == seq;
}

// Hands on next_seq if held, else gives up on it: a lost packet plays as
// silence, so the player's timeline has no hole for its servo to absorb.
static void advance(rtp_t *r) {
    held_t *h = &r->window[r->next_seq % WINDOW];
    if (is_held(r, r->next_seq)) {
        h->held = false;
        deliver(r, h->pcm, h->frames, h->timestamp, h->epoch);
        return;
    }
    if (r->lost++ % LOSS_LOG_EVERY == 0) {
        ESP_LOGW(TAG, "packet %u lost (%lu lost, %lu of %lu resends recovered)", r->next_seq,
                 (unsigned long)r->lost, (unsigned long)r->recovered, (unsigned long)r->requested);
    }
    if (r->have_ts) {
        deliver(r, NULL, r->frames_per_packet, r->next_ts, r->window_epoch);
    } else {
        r->next_seq++;
    }
}

// Whether the packet `ahead` places after next_seq is due within the hole
// deadline (or can't be timed).
static bool due(rtp_t *r, uint16_t ahead, int64_t now) {
    int64_t at_us;
    return !r->have_ts || !playtime_us(r, r->next_ts + (uint32_t)ahead * r->frames_per_packet, &at_us) ||
           at_us - now < HOLE_DEADLINE_US;
}

// Hands on everything held from next_seq in order, and expired holes.
static void release(rtp_t *r) {
    int64_t now = esp_timer_get_time();
    for (;;) {
        // Past the newest packet is the stream's leading edge, not a hole.
        const held_t *h = &r->window[r->next_seq % WINDOW];
        bool held = is_held(r, r->next_seq);
        if (held ? !timed(r, h->timestamp) : (int16_t)(r->highest_seq - r->next_seq) < 0 || !due(r, 0, now)) {
            return;
        }
        advance(r);
    }
}

// Asks for each run of holes that has been missing long enough, wasn't
// asked for recently, and isn't due yet, as player_put_packet() does.
static void request_missing(rtp_t *r) {
    int64_t now = esp_timer_get_time();
    int run_start = -1, run_count = 0;
    for (uint16_t ahead = 0; (int16_t)(r->highest_seq - (uint16_t)(r->next_seq + ahead)) >= 0; ahead++) {
        uint16_t seq = r->next_seq + ahead;
        held_t *h = &r->window[seq % WINDOW];
        bool ask = false;
        if (!is_held(r, seq)) {
            if (h->seq != seq) {
                *h = (held_t){.seq = seq, .noticed_us = now};
            }
            ask = now - h->noticed_us >= RESEND_FIRST_US &&
                  (!h->requested_us || now - h->requested_us >= RESEND_REPEAT_US) && !due(r, ahead, now);
            if (ask) {
                h->requested_us = now;
            }
        }
        if (ask) {
            if (run_start < 0) {
                run_start = seq;
            }
            run_count++;
        } else if (run_count) {
            request_resend(r, (uint16_t)run_start, (uint16_t)run_count);
            run_start = -1;
            run_count = 0;
        }
    }
    if (run_count) {
        request_resend(r, (uint16_t)run_start, (uint16_t)run_count);
    }
}

// The payload after the 12-byte RTP header, deciphered and decoded into
// r->pcm. p and len start at the header's sequence number.
static int decode(rtp_t *r, const uint8_t *p, size_t len) {
    if (!r->ap1) {
        // Ciphertext and tag, then the 8-byte nonce; the timestamp and SSRC
        // are the associated data.
        uint8_t nonce[12] = {0};
        memcpy(nonce + 4, p + len - 8, 8);
        unsigned long long plain_len = 0;
        if (crypto_aead_chacha20poly1305_ietf_decrypt(r->plain, &plain_len, NULL, p + 10, len - 10 - 8, p + 2, 8,
                                                      nonce, r->key) != 0) {
            return -1;
        }
        return decoder_decode(r->dec, r->plain, (size_t)plain_len, r->pcm, DECODER_MAX_FRAMES);
    }

    const uint8_t *payload = p + 10;
    size_t n = len - 10;
    if (r->aes_on) {
        // Whole blocks only, IV reset each packet; the tail is in the clear.
        uint8_t iv[16];
        size_t whole = n & ~(size_t)0xF;
        memcpy(iv, r->aesiv, sizeof(iv));
        mbedtls_aes_crypt_cbc(&r->aes, MBEDTLS_AES_DECRYPT, whole, iv, payload, r->plain);
        memcpy(r->plain + whole, payload + whole, n - whole);
    } else {
        memcpy(r->plain, payload, n);
    }
    if (r->l16) {
        int frames = (int)(n / 4) < r->frames_per_packet ? (int)(n / 4) : r->frames_per_packet;
        for (int i = 0; i < frames * 2; i++) {
            r->pcm[i] = (int32_t)((uint32_t)r->plain[2 * i] << 24 | (uint32_t)r->plain[2 * i + 1] << 16);
        }
        return frames;
    }
    return decoder_decode(r->dec, r->plain, n, r->pcm, DECODER_MAX_FRAMES);
}

static bool long_enough(const rtp_t *r, size_t len) {
    // AirPlay 2 carries a 16-byte tag and 8-byte nonce; AirPlay 1 at least
    // one AES block.
    return r->ap1 ? len >= 10 + 16 && len - 10 <= PACKET_MAX : len > 10 + 16 + 8;
}

// p: sequence number (2 bytes), timestamp (4), SSRC (4), then the payload.
static void put_packet(rtp_t *r, const uint8_t *p, size_t len) {
    if (!long_enough(r, len)) {
        return;
    }
    uint16_t seq = (uint16_t)(p[0] << 8 | p[1]);
    uint32_t timestamp = be32(p + 2);

    xSemaphoreTake(r->audio_lock, portMAX_DELAY);
    r->last_audio_us = esp_timer_get_time();
    r->stall_reported = false;
    if (r->have_floor) {
        if ((int16_t)(seq - r->floor_seq) < 0) {
            xSemaphoreGive(r->audio_lock);
            return;
        }
        r->have_floor = false;
    }
    uint32_t epoch = session_epoch();
    // A flush starts a new sequence; a jump this far ahead is a new stream
    // too. AirPlay 1 marks its restarts with a floor, so far behind is stale.
    int16_t jump = (int16_t)(seq - r->next_seq);
    if (!r->started || epoch != r->window_epoch || jump >= 2 * WINDOW || (!r->ap1 && jump < -2 * WINDOW)) {
        reset_window(r, seq, epoch);
    }
    int16_t ahead = (int16_t)(seq - r->next_seq);
    held_t *h = &r->window[seq % WINDOW];
    // Late resends and duplicates.
    if (ahead < 0 || is_held(r, seq)) {
        xSemaphoreGive(r->audio_lock);
        return;
    }

    if ((int16_t)(seq - r->highest_seq) > 0) {
        r->highest_seq = seq;
    } else {
        r->recovered++;
    }
    // Make room: what this packet's slot still waits for is given up.
    while ((uint16_t)(seq - r->next_seq) >= WINDOW) {
        advance(r);
    }

    int frames = decode(r, p, len);
    if (frames > r->frames_per_packet) {
        ESP_LOGW(TAG, "packet %u: %d frames, expected %d", seq, frames, r->frames_per_packet);
        frames = -1;
    }
    if (frames > 0) {
        if (seq == r->next_seq && timed(r, timestamp)) {
            deliver(r, r->pcm, (size_t)frames, timestamp, epoch);
        } else {
            *h = (held_t){.held = true, .seq = seq, .frames = (uint16_t)frames, .timestamp = timestamp, .epoch = epoch};
            memcpy(h->pcm, r->pcm, (size_t)frames * 2 * sizeof(int32_t));
        }
    }
    release(r);
    request_missing(r);
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

static void handle_control(rtp_t *r, uint8_t *packet, int n, const struct sockaddr_in *from) {
    if (from->sin_port != r->sender_ctrl.sin_port || from->sin_addr.s_addr != r->sender_ctrl.sin_addr.s_addr) {
        xSemaphoreTake(r->audio_lock, portMAX_DELAY);
        r->sender_ctrl = *from;
        xSemaphoreGive(r->audio_lock);
    }
    switch (packet[1]) {
    case ANCHOR_PACKET:
        if (!r->ap1) {
            handle_anchor(packet, (size_t)n);
        }
        break;
    case SYNC_PACKET:
        if (r->ap1) {
            ap1_clock_sync(&r->clock, packet, (size_t)n);
        }
        break;
    case RESENT_AUDIO_PACKET:
        // The original packet behind a 4-byte header.
        put_packet(r, packet + 6, (size_t)n - 6);
        break;
    default:
        ESP_LOGD(TAG, "control packet type 0x%02x, %d bytes", packet[1], n);
        break;
    }
}

static void send_timing_request(rtp_t *r) {
    uint8_t req[AP1_TIMING_REQUEST_LEN];
    ap1_clock_request(req);
    sendto(r->timing_fd, req, sizeof(req), 0, (struct sockaddr *)&r->sender_timing, sizeof(r->sender_timing));
}

static bool ap1_stalled(rtp_t *r, int64_t now) {
    xSemaphoreTake(r->audio_lock, portMAX_DELAY);
    // After a FLUSH the sender goes quiet on purpose: only a stream still
    // playing in the current epoch can stall.
    bool playing = r->started && r->have_ts && r->window_epoch == session_epoch();
    bool stalled = playing && !r->stall_reported && now - r->last_audio_us > AP1_STALL_US;
    if (stalled) {
        r->stall_reported = true;
    }
    xSemaphoreGive(r->audio_lock);
    return stalled;
}

// Quiet: a hole at the end of a burst still expires.
static void expire_holes(rtp_t *r) {
    xSemaphoreTake(r->audio_lock, portMAX_DELAY);
    if (r->started) {
        release(r);
        request_missing(r);
    }
    xSemaphoreGive(r->audio_lock);
}

static int max_fd(const rtp_t *r) {
    int fd = r->control_fd > r->audio_fd ? r->control_fd : r->audio_fd;
    return r->timing_fd > fd ? r->timing_fd : fd;
}

// AirPlay 1's timing requests (a few quickly to sync, then a slow refresh)
// and its stall check.
static void ap1_tick(rtp_t *r, int64_t now, int64_t *next_request_us, int *requests) {
    if (now >= *next_request_us) {
        send_timing_request(r);
        (*requests)++;
        *next_request_us =
            now + (*requests < TIMING_FAST_COUNT || !ap1_clock_timed(&r->clock) ? TIMING_FAST_US : TIMING_SLOW_US);
    }
    if (ap1_stalled(r, now)) {
        ESP_LOGW(TAG, "no audio for %lld s", AP1_STALL_US / 1000000);
        ap2_emit_type(RAOP_EVENT_STALLED);
    }
}

// A realtime stream's ports, all on one task: after an AirPlay 2 session
// internal RAM may not hold a second stack.
static void receive_task(void *arg) {
    rtp_t *r = arg;
    uint8_t *packet = ap2_malloc(PACKET_MAX);
    int nfds = max_fd(r) + 1;
    int64_t next_request_us = 0, last_control_us = esp_timer_get_time();
    int requests = 0;
    bool anchor_lost = false;

    while (packet && !r->stop) {
        int64_t now = esp_timer_get_time();
        if (r->ap1) {
            ap1_tick(r, now, &next_request_us, &requests);
        } else if (!anchor_lost && now - last_control_us >= ANCHOR_TIMEOUT_US) {
            ESP_LOGW(TAG, "no control packets for %lld s, timing lost", ANCHOR_TIMEOUT_US / 1000000);
            session_clear_anchor();
            anchor_lost = true;
        }

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(r->audio_fd, &fds);
        FD_SET(r->control_fd, &fds);
        if (r->timing_fd >= 0) {
            FD_SET(r->timing_fd, &fds);
        }
        struct timeval tv = {.tv_usec = POLL_MS * 1000};
        if (select(nfds, &fds, NULL, NULL, &tv) <= 0) {
            expire_holes(r);
            continue;
        }
        if (r->timing_fd >= 0 && FD_ISSET(r->timing_fd, &fds)) {
            int n = recv(r->timing_fd, packet, PACKET_MAX, 0);
            if (n >= 32 && packet[1] == TIMING_REPLY) {
                ap1_clock_reply(&r->clock, packet, (size_t)n, esp_timer_get_time());
            }
        }
        if (FD_ISSET(r->control_fd, &fds)) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int n = recvfrom(r->control_fd, packet, PACKET_MAX, 0, (struct sockaddr *)&from, &from_len);
            if (n >= 8) {
                last_control_us = esp_timer_get_time();
                anchor_lost = false;
                handle_control(r, packet, n, &from);
            }
        }
        if (FD_ISSET(r->audio_fd, &fds)) {
            int n = recv(r->audio_fd, packet, PACKET_MAX, 0);
            if (n > 12) {
                put_packet(r, packet + 2, (size_t)n - 2);
            }
        } else {
            expire_holes(r);
        }
    }
    ap2_free(packet);
    xSemaphoreGive(r->done);
    ap2_task_exit();
}

static void destroy(rtp_t *r) {
    if (r->control_fd >= 0) {
        close(r->control_fd);
    }
    if (r->audio_fd >= 0) {
        close(r->audio_fd);
    }
    if (r->timing_fd >= 0) {
        close(r->timing_fd);
    }
    decoder_close(r->dec);
    if (r->aes_on) {
        mbedtls_aes_free(&r->aes);
    }
    ap2_free(r->window);
    ap2_free(r->pcm);
    ap2_free(r->plain);
    if (r->audio_lock) {
        vSemaphoreDelete(r->audio_lock);
    }
    if (r->done) {
        vSemaphoreDelete(r->done);
    }
    memset(r->key, 0, sizeof(r->key));
    memset(r->aesiv, 0, sizeof(r->aesiv));
    ap2_free(r);
}

static rtp_t *create(bool realtime, bool ap1) {
    // In PSRAM, leaving internal RAM to the receive task's stack.
    rtp_t *r = ap2_malloc(sizeof(*r));
    if (!r) {
        return NULL;
    }
    memset(r, 0, sizeof(*r));
    r->realtime = realtime;
    r->ap1 = ap1;
    r->frames_per_packet = RT_FRAMES;
    r->audio_fd = r->timing_fd = -1;
    r->audio_lock = xSemaphoreCreateMutex();
    r->done = xSemaphoreCreateBinary();
    return r;
}

static bool alloc_realtime(rtp_t *r) {
    r->pcm = ap2_malloc(DECODER_MAX_FRAMES * 2 * sizeof(int32_t));
    r->plain = ap2_malloc(PACKET_MAX);
    r->window = ap2_malloc(WINDOW * sizeof(held_t));
    if (r->window) {
        memset(r->window, 0, WINDOW * sizeof(held_t));
    }
    return r->pcm && r->plain && r->window;
}

static bool start_task(rtp_t *r) {
    return ap2_task_start(receive_task, "ap2_rtp", TASK_STACK, r, TASK_PRIO, TASK_CORE, false);
}

rtp_t *rtp_start(const uint8_t key[32], bool realtime, uint16_t *data_port, uint16_t *control_port) {
    rtp_t *r = create(realtime, false);
    if (!r) {
        return NULL;
    }
    memcpy(r->key, key, sizeof(r->key));
    r->control_fd = ap2_listen_any(SOCK_DGRAM, control_port);
    bool ok = r->control_fd >= 0 && r->audio_lock && r->done;
    if (ok && realtime) {
        r->audio_fd = ap2_listen_any(SOCK_DGRAM, data_port);
        r->dec = decoder_open_alac();
        ok = r->audio_fd >= 0 && r->dec && alloc_realtime(r) && start_task(r);
    }
    if (!ok) {
        destroy(r);
        return NULL;
    }
    return r;
}

// Fields the sender leaves out keep shairport-sync's defaults: 352-frame,
// 16-bit stereo ALAC at 44.1 kHz.
static void parse_fmtp(const char *fmtp, int out[12]) {
    static const int defaults[12] = {96, RT_FRAMES, 0, 16, 40, 10, 14, 2, 255, 0, 0, AP2_RATE};
    memcpy(out, defaults, sizeof(defaults));
    char *copy = strdup(fmtp ? fmtp : "");
    char *cursor = copy, *arg;
    int n = 0;
    while (copy && n < 12 && (arg = strsep(&cursor, " \t")) != NULL) {
        if (*arg) {
            out[n++] = atoi(arg);
        }
    }
    free(copy);
}

static bool open_ap1_decoder(rtp_t *r, const rtp_ap1_t *p) {
    // pyatv sends an fmtp line with L16 too; L16 is always 352 frames.
    r->l16 = p->rtpmap && strcasestr(p->rtpmap, "L16/");
    if (r->l16) {
        if (!strcasestr(p->rtpmap, "L16/44100/2")) {
            ESP_LOGE(TAG, "unsupported PCM format: %s", p->rtpmap);
            return false;
        }
        return true;
    }
    int fmtp[12];
    parse_fmtp(p->fmtp, fmtp);
    if (fmtp[1] <= 0 || fmtp[1] > RT_FRAMES) {
        ESP_LOGE(TAG, "unsupported fmtp: %s", p->fmtp ? p->fmtp : "(none)");
        return false;
    }
    r->frames_per_packet = (uint16_t)fmtp[1];
    r->dec = decoder_open_alac_fmtp(fmtp);
    return r->dec != NULL;
}

rtp_t *rtp_start_ap1(const rtp_ap1_t *p, uint16_t ports[3]) {
    rtp_t *r = create(true, true);
    if (!r) {
        return NULL;
    }
    ap1_clock_init(&r->clock, p->min_latency, p->max_latency, p->latency_offset);
    r->sender_ctrl = (struct sockaddr_in){
        .sin_family = AF_INET, .sin_addr = p->sender, .sin_port = htons(p->control_port)};
    r->sender_timing = (struct sockaddr_in){
        .sin_family = AF_INET, .sin_addr = p->sender, .sin_port = htons(p->timing_port)};
    if (p->aeskey) {
        mbedtls_aes_init(&r->aes);
        mbedtls_aes_setkey_dec(&r->aes, p->aeskey, 128);
        memcpy(r->aesiv, p->aesiv, sizeof(r->aesiv));
        r->aes_on = true;
    }
    r->control_fd = ap2_listen_any(SOCK_DGRAM, &ports[0]);
    r->timing_fd = ap2_listen_any(SOCK_DGRAM, &ports[1]);
    r->audio_fd = ap2_listen_any(SOCK_DGRAM, &ports[2]);
    bool ok = r->control_fd >= 0 && r->timing_fd >= 0 && r->audio_fd >= 0 && r->audio_lock && r->done &&
              open_ap1_decoder(r, p) && alloc_realtime(r) && start_task(r);
    if (!ok) {
        destroy(r);
        return NULL;
    }
    return r;
}

void rtp_ap1_flush(rtp_t *r, uint16_t seq) {
    xSemaphoreTake(r->audio_lock, portMAX_DELAY);
    r->have_floor = true;
    r->floor_seq = seq;
    xSemaphoreGive(r->audio_lock);
}

void rtp_stop(rtp_t *r) {
    if (r->realtime) {
        r->stop = true;
        xSemaphoreTake(r->done, portMAX_DELAY);
        ESP_LOGI(TAG, "stream done: %lu lost, %lu of %lu resends recovered, min lead %lld ms", (unsigned long)r->lost,
                 (unsigned long)r->recovered, (unsigned long)r->requested,
                 r->min_lead_us == INT64_MAX ? -1LL : (long long)(r->min_lead_us / 1000));
    }
    destroy(r);
}
