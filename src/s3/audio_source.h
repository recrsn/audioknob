#pragma once

// Single-active-source arbitration (spec.md §1) and the CH445P I2S switch
// it drives. Policy is "last source to start playing wins": a newly
// playing source takes the DAC, and the one it displaced is told to stop
// (AirPlay via airplay_preempt(), A2DP via a CMD_PAUSE frame to the ESP32).

typedef enum {
    AUDIO_SOURCE_ESP32,    // A2DP on the wireless MCU; also the idle default
    AUDIO_SOURCE_AIRPLAY,  // RAOP on this MCU
} audio_source_t;

// Drives GPIO0 (call only after boot -- it is a strap pin) and starts
// watching the ESP32's UART-reported playback state. on_airplay_preempted
// runs from a timer task when A2DP starts while AirPlay holds the DAC; it
// must not block.
void audio_source_init(void (*on_airplay_preempted)(void));

void audio_source_claim_airplay(void);
void audio_source_release_airplay(void);

audio_source_t audio_source_active(void);
