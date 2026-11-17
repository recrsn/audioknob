#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MIC_PROCESS_FRAMES 160

typedef struct mic_process mic_process_t;

// One 10 ms mono block at 16 kHz: DC removal, echo cancellation (optional),
// gain, then ESP-SR NS. Noise level: 0 off, 1 mild, 2 medium, 3 aggressive.
// Allocated only during capture; no speech models or model partition.
mic_process_t *mic_process_create(uint8_t gain_db, uint8_t noise_level, bool echo_cancel);
void mic_process_block(mic_process_t *process, const int16_t *in, int16_t *out);
// What the speaker plays, 16 kHz mono, for the echo canceller; ignored
// without one. Each block cancels against the oldest unused reference.
void mic_process_reference(mic_process_t *process, const int16_t *pcm, size_t frames);
// Time spent waiting for the reference so far, not processing.
int64_t mic_process_reference_wait_us(const mic_process_t *process);
void mic_process_destroy(mic_process_t *process);
