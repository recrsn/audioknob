#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MIC_PROCESS_FRAMES 160

typedef struct mic_process mic_process_t;

// One 10 ms mono block at 16 kHz: DC removal, gain, then ESP-SR NS.
// Noise level: 0 off, 1 mild, 2 medium, 3 aggressive.
// Allocated only during capture; no speech models or model partition.
mic_process_t *mic_process_create(uint8_t gain_db, uint8_t noise_level);
void mic_process_block(mic_process_t *process, const int16_t *in, int16_t *out);
void mic_process_destroy(mic_process_t *process);
