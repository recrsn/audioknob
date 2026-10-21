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
    RAOP_EVENT_SESSION_START,  // principal sender selected; audio may follow
    RAOP_EVENT_PREPARE,        // audio SETUP: release old radio resources before allocating
    RAOP_EVENT_PLAY,           // first accepted audio after RECORD or FLUSH
    RAOP_EVENT_FLUSH,          // pause/seek: drop everything queued downstream
    RAOP_EVENT_SESSION_END,    // TEARDOWN, RTSP connection lost, or raop_abort()
    RAOP_EVENT_STALLED,        // no RTP traffic for 30 s
    RAOP_EVENT_VOLUME,
    RAOP_EVENT_METADATA,
    RAOP_EVENT_ARTWORK,        // JPEG/PNG cover art for the current track; len 0 = none
} raop_event_type_t;

typedef struct {
    raop_event_type_t type;
    union {
        // SESSION_START: the sender's name ("Alex's iPhone"), from its
        // X-Apple-Client-Name header or SETUP; NULL or empty if it sent none.
        // Only valid during the callback.
        struct {
            const char *client;
        } session;
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

// Called from the RTSP or RTP task. Returning false from PREPARE refuses
// audio SETUP (sender gets a 503); other return values are ignored.
typedef bool (*raop_event_cb_t)(const raop_event_t *event);

// Decoded interleaved stereo, left-justified 32-bit (NULL: silence), in
// order, as soon as it is available -- typically ~2 s before it should be
// heard. playtime_us is when the first frame must reach the DAC, on the
// esp_timer_get_time() clock. rate is the stream's, 44.1 or 48 kHz; it may
// change between calls.
typedef void (*raop_pcm_cb_t)(const int32_t *pcm, size_t frames, int64_t playtime_us, uint32_t rate);

typedef struct raop_ctx_s raop_ctx_t;

// Advertises "<name>" over mDNS (mdns_init() must already have run) for
// AirPlay 1 and 2 and starts listening for RTSP on all interfaces.
// mac feeds the mDNS instance name and the Apple-Challenge response. Never
// torn down: the receiver lives for the life of the firmware.
raop_ctx_t *raop_create(const char *name, const uint8_t mac[6], raop_event_cb_t event_cb, raop_pcm_cb_t pcm_cb);

// Re-advertises under a new name; a session in progress is unaffected.
void raop_set_name(raop_ctx_t *ctx, const char *name);

// Offers lossless (24-bit/48 kHz ALAC) to senders, besides AAC; on unless
// turned off. Takes effect for the next sender to connect. Callable before
// raop_create().
void raop_set_lossless(bool on);

// The running stream's encoding ("AAC 48 kHz"), or NULL with none.
const char *raop_stream_format(void);

// Drops the current session (if any) within ~100 ms. Safe from any task,
// including from inside the event callback.
void raop_abort(raop_ctx_t *ctx);

// DACP commands back to the sender ("playpause", "pause", ...). Blocking
// network I/O; false if no remote has been discovered for this session.
bool raop_remote_command(raop_ctx_t *ctx, const char *command);
bool raop_remote_volume(raop_ctx_t *ctx, float volume);

