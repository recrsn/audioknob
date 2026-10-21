/*
 * Buffered audio, after shairport-sync's ap2_buffered_audio_processor.c
 * and utilities/buffered_read.c (Copyright (c) Mike Brady 2025, MIT
 * License): one task moves the TCP stream into a large PSRAM buffer, so
 * the sender can stay far ahead, and another takes blocks out in order,
 * applies FLUSHBUFFERED requests, and decodes each block once its playtime
 * is near.
 */

#include "airplay/receiver/buffered.h"

#include <string.h>

#include "airplay/receiver/ap2.h"
#include "airplay/shared/decoder.h"
#include "esp_heap_caps.h"
#include "esp_freertos_hooks.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "airplay/receiver/session.h"
#include "sodium.h"

#define BUFFER_SIZE (1024 * 1024)
#define BUFFER_SIZE_MIN (256 * 1024)
#define BLOCK_MAX (16 * 1024)
#define RECV_CHUNK 4096
#define MAX_DEFERRED_FLUSHES 25

// A buffered block's SSRC is its encoding, which the sender may change
// between blocks (lossy to lossless at a track change); values after
// shairport-sync's player.h.
#define SSRC_NONE 0
#define SSRC_ALAC_48000_S24_2 0x15000000
#define SSRC_AAC_44100_F24_2 0x16000000
#define SSRC_AAC_48000_F24_2 0x17000000
#define AAC_FRAMES 1024
#define ALAC_FRAMES 352

// Decode a block once it is due within this long, as shairport-sync's
// default 0.75 s decoded-buffer target plus its 0.1 s margin.
#define LEAD_US 850000
#define WAIT_MS 20
#define POLL_MS 100
// After a play or skip the reader receives and the decoder decodes back to
// back, and an ALAC 24-bit burst can take all of core 0: when the idle task
// (task watchdog) hasn't run for this long, each sleeps a tick until it has.
#define IDLE_STARVED_US 20000

#define READER_STACK 3072
// Below the decoder: with a wide TCP window a burst never leaves the reader
// waiting on the socket, and a reader above it starved the decoder into
// late blocks. A full window just pauses the sender.
#define READER_PRIO 6
#define DECODER_STACK 8192
// Core 0, with WiFi and the reader: on core 1 below LVGL (7) the now-playing
// screen left an ALAC decoder most blocks late, and above it the UI would wait.
#define DECODER_PRIO 7
#define DECODER_CORE 0
#define READER_CORE 0

// Decode time per encoding, as wall time: a block preempted counts
// the preemption too, so the minimum and median are the codec's own cost.
#define FORMATS 3
#define COST_BUCKET_US 100
#define COST_BUCKETS 80
typedef struct {
    uint32_t count, over_budget;
    uint64_t total_us;
    int64_t min_us;
    uint16_t hist[COST_BUCKETS];  // the last bucket holds everything slower
} format_cost_t;

// Investigation counters: no per-packet logging, sleeps or priority changes.
// Summaries are emitted only when the decoder exits, to preserve scheduling.
typedef struct {
    uint32_t blocks, decoded, late, flushed, waits, decode_errors;
    uint64_t decode_us;
    int64_t max_decode_us, max_read_us, max_idle_gap_us, last_idle_us;
    uint32_t max_decode_seq;
    unsigned max_packet, max_buffered;
    format_cost_t cost[FORMATS];
} decode_diag_t;

static volatile int64_t s_idle_last_us;
static bool s_diag_registered;
static bool diag_idle(void) {
    s_idle_last_us = esp_timer_get_time();
    return true;
}

// One yield frees the CPU only for the other busy task; the reader and
// decoder each sleeping until the idle task has run lands them asleep at once.
static void yield_to_idle(void) {
    if (esp_timer_get_time() - s_idle_last_us >= IDLE_STARVED_US) {
        vTaskDelay(1);
    }
}

#define ALLOC_FAILURES 8
typedef struct {
    uint32_t ms, caps;
    size_t size, free, largest;
    const char *function;
    char task[16];
} alloc_failure_t;
static alloc_failure_t s_alloc_failures[ALLOC_FAILURES];
static unsigned s_alloc_count;
static portMUX_TYPE s_alloc_lock = portMUX_INITIALIZER_UNLOCKED;

// The heap invokes this after a failed attempt, outside its allocation lock.
// Avoid logging or allocating on this path; dump the fixed ring at stream end.
static void diag_alloc_failed(size_t size, uint32_t caps, const char *function) {
    alloc_failure_t event = {
        .ms = (uint32_t)(esp_timer_get_time() / 1000), .caps = caps, .size = size,
        .free = heap_caps_get_free_size(caps), .largest = heap_caps_get_largest_free_block(caps),
        .function = function,
    };
    const char *name = xPortInIsrContext() ? "ISR" : pcTaskGetName(NULL);
    for (unsigned i = 0; i + 1 < sizeof(event.task) && name[i]; ++i) event.task[i] = name[i];
    portENTER_CRITICAL(&s_alloc_lock);
    s_alloc_failures[s_alloc_count++ % ALLOC_FAILURES] = event;
    portEXIT_CRITICAL(&s_alloc_lock);
}

static void diag_poll(decode_diag_t *d) {
    int64_t now = esp_timer_get_time();
    // Decoder and idle hook both run on DECODER_CORE, so this 64-bit read cannot
    // be interrupted by the lower-priority writer.
    int64_t idle = s_idle_last_us;
    if (idle < d->last_idle_us) idle = d->last_idle_us;
    int64_t gap = now - idle;
    if (gap > d->max_idle_gap_us) d->max_idle_gap_us = gap;
}

static void diag_dump(const decode_diag_t *d) {
    ESP_LOGI("ap2_diag", "blocks=%lu decoded=%lu late=%lu flushed=%lu waits=%lu errors=%lu decode_total_us=%llu max_decode_us=%lld seq=%lu max_read_us=%lld max_idle_gap_us=%lld max_packet=%u max_buffered=%u",
             (unsigned long)d->blocks, (unsigned long)d->decoded, (unsigned long)d->late,
             (unsigned long)d->flushed, (unsigned long)d->waits, (unsigned long)d->decode_errors,
             (unsigned long long)d->decode_us, (long long)d->max_decode_us,
             (unsigned long)d->max_decode_seq, (long long)d->max_read_us,
             (long long)d->max_idle_gap_us, d->max_packet, d->max_buffered);
    static const char *const names[FORMATS] = {"AAC 44.1 kHz", "AAC 48 kHz", "ALAC 24-bit 48 kHz"};
    for (int f = 0; f < FORMATS; ++f) {
        const format_cost_t *c = &d->cost[f];
        if (!c->count) continue;
        int p50 = -1, p90 = -1;
        uint32_t seen = 0;
        for (int i = 0; i < COST_BUCKETS && p90 < 0; ++i) {
            seen += c->hist[i];
            if (p50 < 0 && seen * 2 >= c->count) p50 = i;
            if (seen * 10 >= c->count * 9) p90 = i;
        }
        ESP_LOGI("ap2_diag", "%s: %lu blocks, decode mean %llu us, min %lld us, p50 <%d us, p90 <%d us, %lu over real time",
                 names[f], (unsigned long)c->count, (unsigned long long)(c->total_us / c->count), (long long)c->min_us,
                 (p50 + 1) * COST_BUCKET_US, (p90 + 1) * COST_BUCKET_US, (unsigned long)c->over_budget);
    }
    alloc_failure_t events[ALLOC_FAILURES];
    portENTER_CRITICAL(&s_alloc_lock);
    unsigned count = s_alloc_count;
    memcpy(events, s_alloc_failures, sizeof(events));
    portEXIT_CRITICAL(&s_alloc_lock);
    ESP_LOGI("ap2_diag", "allocation failures since stream start=%u (last %u retained)", count, ALLOC_FAILURES);
    unsigned first = count > ALLOC_FAILURES ? count - ALLOC_FAILURES : 0;
    for (unsigned i = first; i < count; ++i) {
        const alloc_failure_t *e = &events[i % ALLOC_FAILURES];
        ESP_LOGI("ap2_diag", "alloc_fail at_ms=%lu task=%s function=%s size=%u caps=0x%lx free=%u largest=%u",
                 (unsigned long)e->ms, e->task, e->function, (unsigned)e->size,
                 (unsigned long)e->caps, (unsigned)e->free, (unsigned)e->largest);
    }
    ap2_log_heap();
}

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
        yield_to_idle();
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
    uint32_t dec_ssrc;  // what dec decodes
    uint32_t rate, block_frames;
    decode_diag_t diag;
    uint8_t *plain;  // decrypted payload, after DECODER_AAC_HEADROOM bytes
    int32_t *pcm;
    int played;  // blocks played since play (re)started
    uint32_t expected_timestamp;
    bool play_enabled;
    uint32_t late_run;  // late blocks in a row
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
        if (blk->ssrc == SSRC_AAC_44100_F24_2 || blk->ssrc == SSRC_AAC_48000_F24_2 ||
            blk->ssrc == SSRC_ALAC_48000_S24_2) {
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

static int64_t frames_us(int64_t frames, uint32_t rate) {
    return frames * 1000000 / rate;
}

static const char *ssrc_name(uint32_t ssrc) {
    switch (ssrc) {
    case SSRC_ALAC_48000_S24_2:
        return "ALAC 24-bit 48 kHz";
    case SSRC_AAC_48000_F24_2:
        return "AAC 48 kHz";
    default:
        return "AAC 44.1 kHz";
    }
}

// Opens the decoder for this block's encoding if it isn't the one open.
// A new encoding is a new start: its timestamps don't continue the old
// one's, and the player reclocks the DAC if the rate changed.
static bool select_decoder(play_state_t *ps, const block_t *blk) {
    if (ps->dec && ps->dec_ssrc == blk->ssrc) {
        return true;
    }
    if (ps->dec_ssrc != SSRC_NONE) {
        ESP_LOGI(TAG, "block %lu: %s -> %s at timestamp %lu", (unsigned long)blk->seq, ssrc_name(ps->dec_ssrc),
                 ssrc_name(blk->ssrc), (unsigned long)blk->timestamp);
    } else {
        ESP_LOGI(TAG, "%s", ssrc_name(blk->ssrc));
    }
    decoder_close(ps->dec);
    bool alac = blk->ssrc == SSRC_ALAC_48000_S24_2;
    ps->rate = blk->ssrc == SSRC_AAC_44100_F24_2 ? AP2_RATE : 48000;
    ps->block_frames = alac ? ALAC_FRAMES : AAC_FRAMES;
    ps->dec = alac ? decoder_open_alac_s24_48k() : decoder_open_aac(ps->rate);
    ps->dec_ssrc = ps->dec ? blk->ssrc : SSRC_NONE;
    ap2_set_format(ps->dec ? ssrc_name(blk->ssrc) : NULL);
    ps->played = 0;
    return ps->dec != NULL;
}

// Deciphers, decodes and queues one block for playback.
static void play_block(buffered_t *b, play_state_t *ps, const block_t *blk, int64_t playtime_us, uint32_t epoch) {
    int32_t gap = ps->played ? (int32_t)(blk->timestamp - ps->expected_timestamp) : 0;
    // Superseded by blocks already queued.
    if (gap <= -(int32_t)ps->block_frames) {
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

    int64_t started = esp_timer_get_time();
    int frames = decoder_decode(ps->dec, plain, (size_t)plain_len, ps->pcm, DECODER_MAX_FRAMES);
    int64_t elapsed = esp_timer_get_time() - started;
    ps->diag.decode_us += elapsed;
    if (elapsed > ps->diag.max_decode_us) {
        ps->diag.max_decode_us = elapsed;
        ps->diag.max_decode_seq = blk->seq;
    }
    ps->diag.decoded++;
    format_cost_t *cost = &ps->diag.cost[ps->dec_ssrc == SSRC_ALAC_48000_S24_2 ? 2
                                        : ps->dec_ssrc == SSRC_AAC_48000_F24_2 ? 1
                                                                               : 0];
    if (!cost->count || elapsed < cost->min_us) cost->min_us = elapsed;
    cost->count++;
    cost->total_us += elapsed;
    if (elapsed > frames_us(ps->block_frames, ps->rate)) cost->over_budget++;
    int64_t bucket = elapsed / COST_BUCKET_US;
    cost->hist[bucket < COST_BUCKETS ? bucket : COST_BUCKETS - 1]++;
    if (frames <= 0) ps->diag.decode_errors++;
    if (frames <= 0) {
        return;
    }
    if (gap != 0) {
        ESP_LOGI(TAG, "block %lu: timestamp %s by %ld frames", (unsigned long)blk->seq, gap > 0 ? "gap" : "overlap",
                 (long)(gap > 0 ? gap : -gap));
    }
    // AAC's first frame after a start or a discontinuity is priming noise,
    // as shairport-sync found.
    if (ps->dec_ssrc != SSRC_ALAC_48000_S24_2 && (ps->played == 0 || gap != 0)) {
        memset(ps->pcm, 0, (size_t)frames * 2 * sizeof(int32_t));
    }

    int skip = 0;
    if (gap < 0) {
        // Overlap: what's already queued covers the start of this block.
        skip = -gap;
        if (skip >= frames) {
            return;
        }
    } else if (gap > 0 && gap <= MAX_FILL_FRAMES) {
        session_pcm(NULL, (size_t)gap, playtime_us - frames_us(gap, ps->rate), ps->rate, epoch);
    }
    session_pcm(ps->pcm + skip * 2, (size_t)(frames - skip), playtime_us + frames_us(skip, ps->rate), ps->rate,
                epoch);
    ps->played++;
    ps->expected_timestamp = blk->timestamp + (uint32_t)frames;
}

static void decoder_task(void *arg) {
    buffered_t *b = arg;
    play_state_t ps = {
        .plain = heap_caps_malloc(BLOCK_MAX + DECODER_AAC_HEADROOM, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        .pcm = heap_caps_malloc(DECODER_MAX_FRAMES * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
    };
    block_t blk = {.packet = heap_caps_malloc(BLOCK_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)};
    bool need_block = true;
    ps.diag.last_idle_us = esp_timer_get_time();

    if (!ps.plain || !ps.pcm || !blk.packet) {
        ESP_LOGE(TAG, "out of memory");
    } else {
        while (!b->stop) {
            diag_poll(&ps.diag);
            unsigned queued = xStreamBufferBytesAvailable(b->stream);
            if (queued > ps.diag.max_buffered) ps.diag.max_buffered = queued;
            bool enabled = session_play_enabled();
            if (enabled && !ps.play_enabled) {
                need_block = true;
            } else if (!enabled && ps.play_enabled) {
                ps.played = 0;
            }
            ps.play_enabled = enabled;

            if (need_block) {
                int64_t read_start = esp_timer_get_time();
                if (!next_block(b, &blk)) {
                    break;
                }
                int64_t read_us = esp_timer_get_time() - read_start;
                if (read_us > ps.diag.max_read_us) ps.diag.max_read_us = read_us;
                ps.diag.blocks++;
                if (blk.len > ps.diag.max_packet) ps.diag.max_packet = blk.len;
                need_block = false;
            }
            if (apply_flushes(b, &blk)) {
                ps.diag.flushed++;
                need_block = true;
                continue;
            }

            if (!select_decoder(&ps, &blk)) {
                ps.diag.decode_errors++;
                need_block = true;
                continue;
            }
            uint32_t epoch = session_epoch();
            int64_t playtime_us;
            if (!session_playtime_us(blk.timestamp, ps.rate, &playtime_us)) {
                ps.diag.waits++;
                vTaskDelay(pdMS_TO_TICKS(WAIT_MS));
                continue;
            }
            int64_t lead = playtime_us - esp_timer_get_time();
            if (!ps.play_enabled || lead >= LEAD_US) {
                ps.diag.waits++;
                vTaskDelay(pdMS_TO_TICKS(WAIT_MS));
                continue;
            }
            if (lead >= 0) {
                play_block(b, &ps, &blk, playtime_us, epoch);
                ps.late_run = 0;
            } else {
                ps.diag.late++;
                if (ps.late_run++ == 0) {
                    ESP_LOGI(TAG, "block %lu: %lld ms late, %u bytes queued", (unsigned long)blk.seq,
                             (long long)(-lead / 1000), (unsigned)xStreamBufferBytesAvailable(b->stream));
                }
            }
            need_block = true;
            yield_to_idle();
        }
    }

    diag_poll(&ps.diag);
    diag_dump(&ps.diag);
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
    if (!s_diag_registered) {
        ESP_ERROR_CHECK(esp_register_freertos_idle_hook_for_cpu(diag_idle, DECODER_CORE));
        ESP_ERROR_CHECK(heap_caps_register_failed_alloc_callback(diag_alloc_failed));
        s_diag_registered = true;
    }
    portENTER_CRITICAL(&s_alloc_lock);
    s_alloc_count = 0;
    portEXIT_CRITICAL(&s_alloc_lock);
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
    // PSRAM stack: internal RAM runs out during a buffered stream, and the
    // decoder never writes flash. Measured: same average decode time.
    if (!ap2_task_start(decoder_task, "ap2_buf_dec", DECODER_STACK, b, DECODER_PRIO, DECODER_CORE, true)) {
        destroy(b);
        return NULL;
    }
    if (!ap2_task_start(reader_task, "ap2_buf_rx", READER_STACK, b, READER_PRIO, READER_CORE, false)) {
        b->stop = true;
        xSemaphoreTake(b->decoder_done, portMAX_DELAY);
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
