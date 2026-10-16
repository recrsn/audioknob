#pragma once

/*
 * AirPlay audio decoding on esp_audio_codec, to interleaved s16 stereo:
 * ALAC (AirPlay 1 and AirPlay 2 realtime streams) and AAC-LC (AirPlay 2
 * buffered streams), 44.1 kHz.
 */

#include <stddef.h>
#include <stdint.h>

#define DECODER_MAX_FRAMES 1024
// Bytes that must precede an AAC payload in its buffer, for the ADTS header.
#define DECODER_AAC_HEADROOM 7

typedef struct decoder decoder_t;

decoder_t *decoder_open_aac(void);
// AirPlay 2's 352-frame, 16-bit stereo ALAC.
decoder_t *decoder_open_alac(void);
// From the 12 integers of an AirPlay 1 SDP "a=fmtp:" line ("96 352 0 16 40
// 10 14 2 255 0 0 44100": payload type, then the ALACSpecificConfig fields).
// NULL unless it is 16-bit stereo.
decoder_t *decoder_open_alac_fmtp(const int fmtp[12]);
void decoder_close(decoder_t *dec);

// Decodes one packet into out, which holds max_frames stereo frames.
// For AAC, `in` must have DECODER_AAC_HEADROOM writable bytes before it.
// Returns frames decoded, or -1; a failed decoder resets itself.
int decoder_decode(decoder_t *dec, uint8_t *in, size_t len, int16_t *out, size_t max_frames);
