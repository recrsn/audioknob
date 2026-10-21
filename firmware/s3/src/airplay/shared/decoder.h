#pragma once

/*
 * AirPlay audio decoding on esp_audio_codec, to interleaved stereo,
 * left-justified 32-bit: ALAC (AirPlay 1 and AirPlay 2 realtime streams,
 * 16-bit at 44.1 kHz; AirPlay 2 buffered lossless, 24-bit at 48 kHz) and
 * AAC-LC (AirPlay 2 buffered streams, 44.1 or 48 kHz).
 */

#include <stddef.h>
#include <stdint.h>

#define DECODER_MAX_FRAMES 1024
// Bytes that must precede an AAC payload in its buffer, for the ADTS header.
#define DECODER_AAC_HEADROOM 7

typedef struct decoder decoder_t;

decoder_t *decoder_open_aac(uint32_t rate);
// AirPlay 2's 352-frame stereo ALAC: 16-bit at 44.1 kHz, or 24-bit at 48.
decoder_t *decoder_open_alac(void);
decoder_t *decoder_open_alac_s24_48k(void);
// From the 12 integers of an AirPlay 1 SDP "a=fmtp:" line ("96 352 0 16 40
// 10 14 2 255 0 0 44100": payload type, then the ALACSpecificConfig fields).
// NULL unless it is 16- or 24-bit stereo.
decoder_t *decoder_open_alac_fmtp(const int fmtp[12]);
void decoder_close(decoder_t *dec);

// Decodes one packet into out, which holds max_frames stereo frames.
// For AAC, `in` must have DECODER_AAC_HEADROOM writable bytes before it.
// Returns frames decoded, or -1; a failed decoder resets itself.
int decoder_decode(decoder_t *dec, uint8_t *in, size_t len, int32_t *out, size_t max_frames);
