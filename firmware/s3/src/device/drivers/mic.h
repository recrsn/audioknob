#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The PDM mic (MSM261D4030H1CPM, clock GPIO45, data GPIO46) read through
// I2S0's PDM-to-PCM filter as 16 kHz mono, with DC removed. One user at a
// time: push-to-talk now, a memo recorder or voice assistant later.

#define MIC_SAMPLE_RATE 16000

// Frames the DMA holds before the oldest are dropped; read at least this
// often.
#define MIC_BUFFER_MS 160

void mic_init(void);

// Claims the mic and starts its clock; false while someone else holds it.
// Stopped, the mic's clock is off and it sleeps.
bool mic_start(void);
void mic_stop(void);

// Blocks for up to timeout_ms; returns the frames read.
size_t mic_read(int16_t *pcm, size_t frames, uint32_t timeout_ms);

// Debug: the PDM filter's output as is, DC included, and which edge of
// the clock the mic answers on (its L/R select pin), for bring-up.
void mic_set_raw(bool raw);
void mic_set_right_slot(bool right);
