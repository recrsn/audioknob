#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A call's speaker side. The worker is paced by the mic and its frames come
// out unevenly; written straight to the DAC, whose queue holds only ~33 ms,
// a late frame let it run dry. Here frames wait in a short ring that a task
// drains into the DAC, blocking whenever the DAC is full, so the DAC stays
// full and the ring takes up the worker's jitter.

// Writes 44.1 kHz stereo to the DAC, or drops it once the call has lost the
// DAC. Runs on the playout task.
typedef void (*talk_playout_write_t)(const int16_t *pcm, size_t frames);

bool talk_playout_start(talk_playout_write_t write);
// 16 kHz mono, at the speaker's volume. Never blocks.
void talk_playout_push(const int16_t *pcm, size_t frames);
// Waits for the task to finish its current write; logs the ring's gaps.
void talk_playout_stop(void);
