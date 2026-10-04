#pragma once

#include <stddef.h>
#include <stdint.h>

// I2S TX from the S3 to the CH445P's S3-side input (GPIO39/40/41), which
// reaches the PCM5100A only while audio_source has selected the S3 side.
// Fixed at 44.1 kHz / 16-bit / stereo: AirPlay's only format. Underruns
// play silence (DMA auto-clear), so callers never need to pad.

void audio_out_init(void);

// Blocks until all frames are queued to DMA.
void audio_out_write(const int16_t *pcm, size_t frames);

// How long a frame written now takes to reach the DAC: the DMA ring depth.
uint32_t audio_out_latency_ms(void);
