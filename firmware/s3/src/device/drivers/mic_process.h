#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MIC_PROCESS_FRAMES 160

typedef struct mic_process mic_process_t;

// One 10 ms mono block at 16 kHz: DC removal, gain, then ESP-SR NS.
// Allocated only during capture; no speech models or model partition.
mic_process_t *mic_process_create(uint8_t gain_db, bool suppress_noise);
void mic_process_block(mic_process_t *process, const int16_t *in, int16_t *out);
void mic_process_destroy(mic_process_t *process);
