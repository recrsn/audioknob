/*
 * Buffered audio, after shairport-sync's ap2_buffered_audio_processor.c
 * and utilities/buffered_read.c (Copyright (c) Mike Brady 2025, MIT
 * License): one task moves the TCP stream into a large PSRAM buffer, so
 * the sender can stay far ahead, and another takes blocks out in order,
 * applies FLUSHBUFFERED requests, and decodes each block once its playtime
 * is near.
 */

#ifndef AK_NATIVE

#include "buffered.h"

#include <string.h>

#include "ap2.h"
#include "decoder.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "session.h"
#include "sodium.h"

#define BUFFER_SIZE (1024 * 1024)
#define BUFFER_SIZE_MIN (256 * 1024)
#define BLOCK_MAX (16 * 1024)
#define RECV_CHUNK 4096
#define MAX_DEFERRED_FLUSHES 25

#define SSRC_NONE 0
#define SSRC_AAC_44100_F24_2 0x16000000
#define AAC_FRAMES 1024

// Decode a block once it is due within this long, as shairport-sync's
// default 0.75 s decoded-buffer target plus its 0.1 s margin.
#define LEAD_US 850000
#define WAIT_MS 20
#define POLL_MS 100

#define READER_STACK 3072
#define READER_PRIO 8
#define DECODER_STACK 8192
#define DECODER_PRIO 7
#define TASK_CORE 0

typedef struct {
    bool in_use, active;
    uint32_t from_seq, until_seq;
} deferred_flush_t;

struct buffered {
    uint8_t key[32];
    int listen_fd;
    volatile int data_fd;
    volatile bool stop, closed;

    StreamBufferHandle_t stream;
    StaticStreamBuffer_t stream_struct;
    uint8_t *storage;

    SemaphoreHandle_t flush_lock;
    bool immediate_flush;
    uint32_t immediate_until_seq;
    deferred_flush_t deferred[MAX_DEFERRED_FLUSHES];

    SemaphoreHandle_t reader_done, decoder_done;
};

typedef struct {
    uint8_t *packet;
    size_t len;
    uint32_t seq, timestamp, ssrc;
} block_t;

static const char *TAG = "ap2_buffered";

// Signed difference of two 23-bit sequence numbers.
static int32_t seq_diff(uint32_t a, uint32_t b) {
    uint32_t d = (a - b) & 0x7FFFFF;
    return (d & 0x400000) ? (int32_t)(d | 0xFF800000) : (int32_t)d;
}

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Reader */

static void reader_task(void *arg) {
    buffered_t *b = arg;
    uint8_t *chunk = ap2_malloc(RECV_CHUNK);

    while (chunk && !b->stop && b->data_fd < 0) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(b->listen_fd, &set);
        struct timeval tv = {.tv_usec = POLL_MS * 1000};
        if (select(b->listen_fd + 1, &set, NULL, NULL, &tv) > 0) {
            int fd = accept(b->listen_fd, NULL, NULL);
            if (fd >= 0) {
                struct timeval rto = {.tv_usec = POLL_MS * 1000};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rto, sizeof(rto));
                b->data_fd = fd;
            }
        }
    }

    while (chunk && !b->stop) {
        int n = ap2_recv(b->data_fd, chunk, RECV_CHUNK, &b->stop);
        if (n <= 0) {
            break;
        }
        size_t sent = 0;
        while (sent < (size_t)n && !b->stop) {
            sent += xStreamBufferSend(b->stream, chunk + sent, (size_t)n - sent, pdMS_TO_TICKS(POLL_MS));
        }
    }
    if (!b->stop) {
        ESP_LOGI(TAG, "sender closed the audio connection");
    }
    ap2_free(chunk);
    b->closed = true;
    xSemaphoreGive(b->reader_done);
    ap2_task_exit();
}

static bool read_exact(buffered_t *b, void *buf, size_t len) {
    size_t have = 0;
    while (have < len) {
        size_t n = xStreamBufferReceive(b->stream, (uint8_t *)buf + have, len - have, pdMS_TO_TICKS(POLL_MS));
        have += n;
        if (n == 0 && (b->stop || (b->closed && xStreamBufferIsEmpty(b->stream)))) {
            return false;
        }
    }
    return true;
}

/* Flushes */

void buffered_flush(buffered_t *b, bool from_valid, uint32_t from_seq, uint32_t until_seq) {
    xSemaphoreTake(b->flush_lock, portMAX_DELAY);
    if (!from_valid) {
        b->immediate_flush = true;
        b->immediate_until_seq = until_seq & 0x7FFFFF;
    } else {
        int i = 0;
        while (i < MAX_DEFERRED_FLUSHES && b->deferred[i].in_use) {
            i++;
        }
        if (i < MAX_DEFERRED_FLUSHES) {
            b->deferred[i] = (deferred_flush_t){
                .in_use = true,
                .from_seq = from_seq & 0x7FFFFF,
                .until_seq = until_seq & 0x7FFFFF,
            };
        } else {
            ESP_LOGW(TAG, "no room for another deferred flush");
        }
    }
    xSemaphoreGive(b->flush_lock);
}

// True if the block is flushed and should be dropped.
static bool apply_flushes(buffered_t *b, const block_t *blk) {
    bool drop = false;
    xSemaphoreTake(b->flush_lock, portMAX_DELAY);
    if (b->immediate_flush) {
        bool finished = b->immediate_until_seq == 0 || seq_diff(blk->seq, b->immediate_until_seq) >= 0;
        if (finished) {
            b->immediate_flush = false;
            // An immediate flush supersedes deferred ones not yet started.
            for (int i = 0; i < MAX_DEFERRED_FLUSHES; i++) {
                if (b->deferred[i].in_use && !b->deferred[i].active) {
                    b->deferred[i].in_use = false;
                }
            }
        } else {
            drop = true;
        }
    }
    for (int i = 0; i < MAX_DEFERRED_FLUSHES; i++) {
        deferred_flush_t *f = &b->deferred[i];
        if (!f->in_use) {
            continue;
        }
        if (f->from_seq == blk->seq && f->until_seq != blk->seq) {
            f->active = true;
        }
        if (f->until_seq == blk->seq || seq_diff(blk->seq, f->until_seq) > 0) {
            f->in_use = false;
            f->active = false;
        } else if (f->active) {
            drop = true;
        }
    }
    xSemaphoreGive(b->flush_lock);
    return drop;
}

/* Decoder */

typedef struct {
    decoder_t *dec;
    uint8_t *plain;  // decrypted payload, after DECODER_AAC_HEADROOM bytes
    int16_t *pcm;
    int played;  // blocks played since play (re)started
    uint32_t expected_timestamp;
    bool play_enabled;
} play_state_t;

static bool next_block(buffered_t *b, block_t *blk) {
    for (;;) {
        uint8_t len_be[2];
        if (!read_exact(b, len_be, sizeof(len_be))) {
            return false;
        }
        size_t len = (size_t)(len_be[0] << 8 | len_be[1]);
        if (len < 2) {
            continue;
        }
        len -= 2;
        if (len > BLOCK_MAX) {
            ESP_LOGE(TAG, "%u byte block: stream out of step", (unsigned)len);
            return false;
        }
        if (!read_exact(b, blk->packet, len)) {
            return false;
        }
        if (len < 12 + 16 + 8) {
            continue;
        }
        blk->len = len;
        blk->seq = be32(blk->packet) & 0x7FFFFF;
        blk->timestamp = be32(blk->packet + 4);
        blk->ssrc = be32(blk->packet + 8);
        if (blk->ssrc == SSRC_AAC_44100_F24_2) {
            return true;
        }
        if (blk->ssrc != SSRC_NONE) {
            ESP_LOGW(TAG, "block %lu: unsupported encoding 0x%08lx", (unsigned long)blk->seq,
                     (unsigned long)blk->ssrc);
        }
    }
}

// Fills a timestamp gap up to this long with silence, so the queue (which
// plays blocks back to back) stays on the sender's timeline; longer ones
// are a new start and the player re-aligns to them.
#define MAX_FILL_FRAMES (4 * AAC_FRAMES)

static const int16_t s_silence[AAC_FRAMES * 2];

static int64_t frames_us(int64_t frames) {
    return frames * 1000000 / AP2_RATE;
}

// Deciphers, decodes and queues one block for playback.
static void play_block(buffered_t *b, play_state_t *ps, const block_t *blk, int64_t playtime_us, uint32_t epoch) {
    int32_t gap = ps->played ? (int32_t)(blk->timestamp - ps->expected_timestamp) : 0;
    // Superseded by blocks already queued.
    if (gap <= -AAC_FRAMES) {
        return;
    }

    uint8_t nonce[12] = {0};
    memcpy(nonce + 4, blk->packet + blk->len - 8, 8);
    unsigned long long plain_len = 0;
    uint8_t *plain = ps->plain + DECODER_AAC_HEADROOM;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(plain, &plain_len, NULL, blk->packet + 12, blk->len - 12 - 8,
                                                  blk->packet + 4, 8, nonce, b->key) != 0) {
        ESP_LOGW(TAG, "block %lu: can't decipher", (unsigned long)blk->seq);
        return;
    }

    int frames = decoder_decode(ps->dec, plain, (size_t)plain_len, ps->pcm, DECODER_MAX_FRAMES);
    if (frames <= 0) {
        return;
    }
    if (gap != 0) {
        ESP_LOGI(TAG, "block %lu: timestamp %s by %ld frames", (unsigned long)blk->seq, gap > 0 ? "gap" : "overlap",
                 (long)(gap > 0 ? gap : -gap));
    }
    // The decoder's first frame after a start or a discontinuity is priming
    // noise, as shairport-sync found.
    if (ps->played == 0 || gap != 0) {
        memset(ps->pcm, 0, (size_t)frames * 2 * sizeof(int16_t));
    }

    int skip = 0;
    if (gap < 0) {
        // Overlap: what's already queued covers the start of this block.
        skip = -gap;
        if (skip >= frames) {
            return;
        }
    } else if (gap > 0 && gap <= MAX_FILL_FRAMES) {
        int64_t at = playtime_us - frames_us(gap);
        for (int32_t left = gap; left > 0;) {
            int32_t n = left < AAC_FRAMES ? left : AAC_FRAMES;
            session_pcm(s_silence, (size_t)n, at, epoch);
            at += frames_us(n);
            left -= n;
        }
    }
    session_pcm(ps->pcm + skip * 2, (size_t)(frames - skip), playtime_us + frames_us(skip), epoch);
    ps->played++;
    ps->expected_timestamp = blk->timestamp + (uint32_t)frames;
}

static void decoder_task(void *arg) {
    buffered_t *b = arg;
    play_state_t ps = {
        .dec = decoder_open_aac(),
        .plain = heap_caps_malloc(BLOCK_MAX + DECODER_AAC_HEADROOM, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .pcm = heap_caps_malloc(DECODER_MAX_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };
    block_t blk = {.packet = heap_caps_malloc(BLOCK_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)};
    bool need_block = true;

    if (!ps.dec || !ps.plain || !ps.pcm || !blk.packet) {
        ESP_LOGE(TAG, "out of memory");
    } else {
        while (!b->stop) {
            bool enabled = session_play_enabled();
            if (enabled && !ps.play_enabled) {
                need_block = true;
            } else if (!enabled && ps.play_enabled) {
                ps.played = 0;
            }
            ps.play_enabled = enabled;

            if (need_block) {
                if (!next_block(b, &blk)) {
                    break;
                }
                need_block = false;
            }
            if (apply_flushes(b, &blk)) {
                need_block = true;
                continue;
            }

            uint32_t epoch = session_epoch();
            int64_t playtime_us;
            if (!session_playtime_us(blk.timestamp, &playtime_us)) {
                vTaskDelay(pdMS_TO_TICKS(WAIT_MS));
                continue;
            }
            int64_t lead = playtime_us - esp_timer_get_time();
            if (!ps.play_enabled || lead >= LEAD_US) {
                vTaskDelay(pdMS_TO_TICKS(WAIT_MS));
                continue;
            }
            if (lead >= 0) {
                play_block(b, &ps, &blk, playtime_us, epoch);
            }
            need_block = true;
        }
    }

    decoder_close(ps.dec);
    heap_caps_free(ps.plain);
    heap_caps_free(ps.pcm);
    heap_caps_free(blk.packet);
    xSemaphoreGive(b->decoder_done);
    ap2_task_exit();
}

/* Lifecycle */

static void destroy(buffered_t *b) {
    if (b->listen_fd >= 0) {
        close(b->listen_fd);
    }
    if (b->data_fd >= 0) {
        close(b->data_fd);
    }
    if (b->stream) {
        vStreamBufferDelete(b->stream);
    }
    heap_caps_free(b->storage);
    if (b->flush_lock) {
        vSemaphoreDelete(b->flush_lock);
    }
    if (b->reader_done) {
        vSemaphoreDelete(b->reader_done);
    }
    if (b->decoder_done) {
        vSemaphoreDelete(b->decoder_done);
    }
    memset(b->key, 0, sizeof(b->key));
    heap_caps_free(b);
}

buffered_t *buffered_start(const uint8_t key[32], uint16_t *port, size_t *buffer_size) {
    buffered_t *b = heap_caps_calloc(1, sizeof(*b), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!b) {
        return NULL;
    }
    memcpy(b->key, key, sizeof(b->key));
    b->data_fd = -1;

    // As large as PSRAM allows: the sender sends as far ahead as this.
    size_t size = BUFFER_SIZE;
    while (size >= BUFFER_SIZE_MIN && !(b->storage = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM))) {
        size /= 2;
    }
    b->listen_fd = ap2_listen_any(SOCK_STREAM, port);
    b->flush_lock = xSemaphoreCreateMutex();
    b->reader_done = xSemaphoreCreateBinary();
    b->decoder_done = xSemaphoreCreateBinary();
    if (b->storage) {
        b->stream = xStreamBufferCreateStatic(size, 1, b->storage, &b->stream_struct);
    }
    if (!b->stream || b->listen_fd < 0 || !b->flush_lock || !b->reader_done || !b->decoder_done) {
        destroy(b);
        return NULL;
    }
    if (!ap2_task_start(reader_task, "ap2_buf_rx", READER_STACK, b, READER_PRIO, TASK_CORE, false)) {
        destroy(b);
        return NULL;
    }
    if (!ap2_task_start(decoder_task, "ap2_buf_dec", DECODER_STACK, b, DECODER_PRIO, TASK_CORE, false)) {
        b->stop = true;
        xSemaphoreTake(b->reader_done, portMAX_DELAY);
        destroy(b);
        return NULL;
    }
    *buffer_size = size;
    ESP_LOGI(TAG, "listening on %u, %u KB buffer", *port, (unsigned)(size / 1024));
    return b;
}

void buffered_stop(buffered_t *b) {
    b->stop = true;
    xSemaphoreTake(b->reader_done, portMAX_DELAY);
    xSemaphoreTake(b->decoder_done, portMAX_DELAY);
    destroy(b);
}

#endif  // !AK_NATIVE
