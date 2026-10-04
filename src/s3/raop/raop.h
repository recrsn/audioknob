/*
 * AirPlay 1 (RAOP) receiver: mDNS advertisement, RTSP session handling,
 * and DACP remote control back to the sender.
 *
 * Ported from philippe44's squeezelite-esp32 raop component
 * (c) Philippe 2019-2020, philippe_44@outlook.com -- MIT License,
 * https://opensource.org/licenses/MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RAOP_SAMPLE_RATE 44100

typedef enum {
    RAOP_EVENT_SESSION_START,  // SETUP: a sender is about to stream
    RAOP_EVENT_PLAY,           // first accepted audio after RECORD or FLUSH
    RAOP_EVENT_FLUSH,          // pause/seek: drop everything queued downstream
    RAOP_EVENT_SESSION_END,    // TEARDOWN, RTSP connection lost, or raop_abort()
    RAOP_EVENT_STALLED,        // no RTP traffic for 30 s
    RAOP_EVENT_VOLUME,
    RAOP_EVENT_METADATA,
    RAOP_EVENT_ARTWORK,        // JPEG cover art for the current track
} raop_event_type_t;

typedef struct {
    raop_event_type_t type;
    union {
        // 0.0 = muted, else linear over AirPlay's -30..0 dB slider range.
        float volume;
        struct {
            const char *title, *artist, *album;
        } metadata;
        // Only valid during the callback.
        struct {
            const uint8_t *data;
            size_t len;
        } artwork;
    };
} raop_event_t;

// Called from the RTSP or RTP task. Returning false from SESSION_START
// refuses the stream (sender gets a 503); other return values are ignored.
typedef bool (*raop_event_cb_t)(const raop_event_t *event);

// Decoded interleaved stereo s16, in order, as soon as it is available --
// typically ~2 s before it should be heard. playtime_ms is when the first
// frame must reach the DAC, on the raop_time_ms() clock.
typedef void (*raop_pcm_cb_t)(const int16_t *pcm, size_t frames, uint32_t playtime_ms);

typedef struct raop_ctx_s raop_ctx_t;

// Advertises "<name>" over mDNS (mdns_init() must already have run) and
// starts listening for RTSP on all interfaces. mac feeds the mDNS instance
// name and the Apple-Challenge response. Never torn down: the receiver
// lives for the life of the firmware.
raop_ctx_t *raop_create(const char *name, const uint8_t mac[6], raop_event_cb_t event_cb, raop_pcm_cb_t pcm_cb);

// Drops the current session (if any) within ~100 ms. Safe from any task,
// including from inside the event callback.
void raop_abort(raop_ctx_t *ctx);

// DACP commands back to the sender ("playpause", "pause", ...). Blocking
// network I/O; false if no remote has been discovered for this session.
bool raop_remote_command(raop_ctx_t *ctx, const char *command);
bool raop_remote_volume(raop_ctx_t *ctx, float volume);

// Monotonic ms clock that playtime_ms is expressed in.
uint32_t raop_time_ms(void);
