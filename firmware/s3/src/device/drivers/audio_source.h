#pragma once

#include <stdbool.h>

// Single-active-source arbitration (spec.md §1) and the CH445P I2S switch
// it drives. Policy is "last source to start playing wins": a newly
// playing source takes the DAC, and the one it displaced is told to stop
// (AirPlay via airplay_preempt(), A2DP via a CMD_PAUSE frame to the ESP32,
// local audio via its own callback).

typedef enum {
    AUDIO_SOURCE_ESP32,    // A2DP on the wireless MCU; also the idle default
    AUDIO_SOURCE_AIRPLAY,  // RAOP on this MCU
    AUDIO_SOURCE_LOCAL,    // audio generated on this MCU (focus sounds)
} audio_source_t;

// Drives GPIO0 (call only after boot -- it is a strap pin) and starts
// watching the ESP32's UART-reported playback state. on_airplay_preempted
// runs from a timer task when A2DP starts while AirPlay holds the DAC; it
// must not block.
void audio_source_init(void (*on_airplay_preempted)(void));

// Claims return false while playback is suspended.
bool audio_source_claim_airplay(void);
void audio_source_release_airplay(void);

// Local audio shares the S3's I2S output with AirPlay, so whoever writes it
// must have stopped by the time on_preempted returns: it runs on the task
// of the stream taking over (AirPlay's or the ESP32 watcher), and may wait
// briefly for the last chunk.
void audio_source_set_local_preempted(void (*on_preempted)(void));
bool audio_source_claim_local(void);
void audio_source_release_local(void);

audio_source_t audio_source_active(void);

// Hold all sources silent during firmware writes and installation. Releasing
// the hold permits new playback but never resumes a paused source.
void audio_source_suspend(bool suspended);
bool audio_source_suspended(void);
