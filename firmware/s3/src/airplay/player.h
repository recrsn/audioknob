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
// pcm is left-justified 32-bit stereo, or NULL for silence. A change of
// rate reclocks the DAC once the old rate's audio has played, and the
// stream re-aligns from there.
void airplay_player_push(const int32_t *pcm, size_t frames, int64_t playtime_us, uint32_t rate);

// Discards everything queued (pause, seek, teardown, preemption).
void airplay_player_flush(void);

// 0-100, applied as a cubic (roughly perceptual) software gain, ramped so
// volume steps don't click.
void airplay_player_set_volume(uint8_t volume_pct);
