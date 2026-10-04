#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// C shim over Apple's ALACDecoder (src/s3/alac/), configured from RAOP's
// SDP `a=fmtp:` line rather than an MP4 magic cookie.
typedef struct alac_dec alac_dec_t;

// fmtp: the 12 integers of "96 352 0 16 40 10 14 2 255 0 0 44100"
// (payload type, frameLength, compatibleVersion, bitDepth, pb, mb, kb,
// numChannels, maxRun, maxFrameBytes, avgBitRate, sampleRate).
alac_dec_t *alac_dec_create(const int fmtp[12]);
void alac_dec_delete(alac_dec_t *dec);

// Decodes one RTP payload into interleaved stereo s16. The decoder's bit
// reader can peek a few bytes past `len`, so `in` must have >= 8 bytes of
// readable slack after it. `out` must hold frameLength * 2 samples.
bool alac_dec_decode(alac_dec_t *dec, uint8_t *in, size_t len, int16_t *out, uint32_t *out_frames);

#ifdef __cplusplus
}
#endif
