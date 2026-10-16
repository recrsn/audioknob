#pragma once

#include <stddef.h>
#include <stdint.h>

// I2S TX from the S3 to the CH445P's S3-side input (GPIO39/40/41), which
// reaches the PCM5100A only while audio_source has selected the S3 side.
// Fixed at 44.1 kHz / 16-bit / stereo: AirPlay's only format. Underruns
// play silence (DMA auto-clear), so callers never need to pad.

#define AUDIO_OUT_SAMPLE_RATE 44100

void audio_out_init(void);

// Blocks until all frames are queued to DMA. Single writer only.
void audio_out_write(const int16_t *pcm, size_t frames);

// How long a frame written now takes to reach the DAC, measured from the
// DMA completion cursor rather than modelled, so it is right after an
// underrun too. now_us is esp_timer_get_time() sampled with it.
uint32_t audio_out_queued_us(int64_t *now_us);

// Times the DMA ran dry since boot (each one played as silence).
uint32_t audio_out_underruns(void);
