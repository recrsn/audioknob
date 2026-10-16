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

#define MIC_DEFAULT_GAIN_DB 30
#define MIC_MAX_GAIN_DB 36
#define MIC_DEFAULT_NOISE_LEVEL 1
#define MIC_MAX_NOISE_LEVEL 3

typedef struct {
    uint8_t gain_db;
    uint8_t noise_level; // 0 off, 1 mild, 2 medium, 3 aggressive
    bool raw;
    bool right_slot;
} mic_config_t;

void mic_init(void);

// Claims the mic and starts its clock; NULL config uses +30 dB and ESP-SR
// mild noise suppression. Raw capture bypasses all processing. False if busy
// or the capture/processing buffers cannot be allocated.
bool mic_start(const mic_config_t *config);
void mic_stop(void);

// Blocks for up to timeout_ms; returns the frames read. Processed capture
// keeps partial 10 ms blocks across calls, so callers may request any size.
size_t mic_read(int16_t *pcm, size_t frames, uint32_t timeout_ms);

// Cost of the latest capture, as a share of one core, excluding I2S waits.
float mic_processing_cpu_pct(void);
