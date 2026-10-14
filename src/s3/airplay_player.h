#pragma once

#include <stddef.h>
#include <stdint.h>

// Holds AirPlay PCM until its sender-assigned playtime (RAOP's ~2 s
// latency) and feeds audio_out on that schedule. It starts each stream on
// the exact sample, then keeps the DAC clock locked to the sender's by
// dropping or repeating single samples at a frame's quietest point, which
// is inaudible; only a large disturbance (stall, clock jump) drops whole
// packets or inserts silence.

void airplay_player_init(void);

// Called from the RTP task, in order. Non-blocking: drops audio if the
// queue is somehow full. playtime_us is on the esp_timer_get_time() clock.
void airplay_player_push(const int16_t *pcm, size_t frames, int64_t playtime_us);

// Discards everything queued (pause, seek, teardown, preemption).
void airplay_player_flush(void);

// 0-100, applied as a cubic (roughly perceptual) software gain, ramped so
// volume steps don't click.
void airplay_player_set_volume(uint8_t volume_pct);
