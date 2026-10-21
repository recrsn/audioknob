#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "stream_net.h"

// Plays an internet audio stream (MP3 or AAC over HTTP/HTTPS, ICY or
// playlist) into audio_out, converted to its 44.1 kHz stereo, with a
// software volume. Reconnects with a backoff when the stream drops, and
// keeps a few seconds buffered in PSRAM. Knows nothing about the DAC switch
// or other sources: whoever calls stream_player_play() must hold the DAC
// (audio_source) while it plays, since audio_out takes one writer.
//
// Its two PSRAM-stack tasks exist only while something plays. All sources
// share audio_out's persistent I2S DMA ring.

typedef enum {
    STREAM_IDLE,
    STREAM_CONNECTING,
    STREAM_BUFFERING,
    STREAM_PLAYING,
    STREAM_RETRYING, // lost the stream; reconnecting after a backoff
    STREAM_FAILED,   // gave up on this URL: `error` says why
} stream_state_t;

typedef struct {
    stream_state_t state;
    char url[STREAM_URL_MAX];
    char title[128]; // ICY StreamTitle; "" when the station sends none
    char name[64];   // icy-name; "" when not sent
    char error[64];  // why it's retrying or failed
    stream_format_t format;
    uint32_t sample_rate;
    uint8_t channels;
    uint16_t bitrate_kbps;
} stream_status_t;

void stream_player_init(void);

// Starts (or switches to) `url`; non-blocking.
void stream_player_play(const char *url);
// Silences output at once; the tasks wind down on their own. Non-blocking.
void stream_player_stop(void);
// Wait for decoder, HTTP/TLS and ring teardown. Use only from a worker task,
// after stop; false on timeout or if a new play superseded that stop.
bool stream_player_wait_stopped(uint32_t timeout_ms);

// 0-100 %, cubic like AirPlay's.
void stream_player_set_volume(uint8_t volume_pct);

// Snapshot; returns a version that changes whenever anything in it does.
uint32_t stream_player_status(stream_status_t *out);
