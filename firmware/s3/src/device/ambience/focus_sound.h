#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "ambience_synth.h"

// Ambient focus sounds (ambience_synth.c), generated live on this MCU and
// played through audio_out. Playing claims the DAC as audio_source's local
// source -- pausing A2DP, preempting AirPlay -- and a stream that starts
// meanwhile stops them at once ("last to start playing wins"). Safe from
// any task, including Pomodoro: the ambience app owns the worker and state,
// rather than restricting who may request playback. The worker, synth and
// render buffers exist only on demand, through fade-out or source preemption.
// Allocation failure logs a warning and clears playing; a later play can retry.

void focus_sound_init(void);

// Fades `kind` in, or fades over to it from the one playing.
void focus_sound_play(ambience_kind_t kind);
// Fades out, then hands the DAC back.
void focus_sound_stop(void);
// 0-100, the same cubic gain as AirPlay's, ramped.
void focus_sound_set_volume(uint8_t pct);

// True from play until a stop or a stream taking over, with what's playing.
bool focus_sound_playing(ambience_kind_t *kind);

// Share of a core the synth took over its last 10 s of playing, in %.
float focus_sound_cpu_pct(void);
