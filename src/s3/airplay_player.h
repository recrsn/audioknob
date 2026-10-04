#pragma once

#include <stddef.h>
#include <stdint.h>

// Holds AirPlay PCM until its sender-assigned playtime (RAOP's ~2 s
// latency) and feeds audio_out on that schedule, dropping or padding a few
// ms now and then so the DAC clock never drifts away from the sender's.

void airplay_player_init(void);

// Called from the RTP task, in order. Non-blocking: drops audio if the
// queue is somehow full.
void airplay_player_push(const int16_t *pcm, size_t frames, uint32_t playtime_ms);

// Discards everything queued (pause, seek, teardown, preemption).
void airplay_player_flush(void);

// 0-100, applied as a cubic (roughly perceptual) software gain.
void airplay_player_set_volume(uint8_t volume_pct);
