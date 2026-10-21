#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Pure text parsing for the stream client: playlists, ICY metadata and
// the stream's format. No I/O.

typedef enum {
    STREAM_FORMAT_UNKNOWN,
    STREAM_FORMAT_MP3,
    STREAM_FORMAT_AAC, // ADTS
    STREAM_FORMAT_PLS,
    STREAM_FORMAT_M3U,
} stream_format_t;

// From a Content-Type header ("audio/mpeg; charset=..." included), else
// from the URL's extension; UNKNOWN when neither says.
stream_format_t stream_format_detect(const char *content_type, const char *url);

// From the first bytes of the body: an ID3 tag or MPEG audio frame sync,
// or an ADTS header.
stream_format_t stream_format_sniff(const uint8_t *data, size_t len);

// The first stream URL in a .pls or .m3u body (NUL-terminated). False when
// there is none, or when it's an HLS playlist (`hls` set), which needs a
// segment fetcher rather than one long stream.
bool stream_playlist_first_url(stream_format_t format, const char *body, char *url, size_t len, bool *hls);

// Copies `n` bytes of header or metadata text as UTF-8: as is when they
// already are, else read as Latin-1, which older stations send. Cut on a
// character boundary to fit `len`.
void stream_text_utf8(const char *in, size_t n, char *out, size_t len);

// StreamTitle='...'; out of an ICY metadata block, through
// stream_text_utf8(). False when the block has no title.
bool stream_icy_title(const char *meta, size_t len, char *out, size_t out_len);
