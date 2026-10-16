#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "stream_parse.h"
#include "stream_ring.h"

// One HTTP(S) connection to an internet radio stream: follows redirects and
// .pls/.m3u playlists to the audio, asks for ICY metadata and strips it out
// of the body, and writes the audio into a ring until told to stop or the
// stream breaks. Reconnecting is the caller's call.

#define STREAM_URL_MAX 256

typedef enum {
    STREAM_NET_STOPPED, // should_stop() said so
    STREAM_NET_RETRY,   // lost or couldn't reach it; may work again
    STREAM_NET_FAILED,  // won't work as is (not found, unsupported format)
} stream_net_result_t;

typedef struct {
    stream_format_t format;
    char name[64];  // icy-name, UTF-8; "" when not sent
    int bitrate_kbps; // icy-br; 0 when not sent
} stream_info_t;

typedef struct {
    void *ctx;
    bool (*should_stop)(void *ctx);
    // The audio starts: called once, before the first byte is written to
    // the ring, so the caller can start a new epoch for it.
    void (*on_audio)(void *ctx, const stream_info_t *info);
    void (*on_title)(void *ctx, const char *title);
} stream_net_ops_t;

// Blocks until the stream ends. `error` gets a short reason for RETRY and
// FAILED, fit for the screen.
stream_net_result_t stream_net_run(const char *url, stream_ring_t *ring, const stream_net_ops_t *ops, char *error,
                                   size_t error_len);
