#pragma once

#include <stdbool.h>

// Single-active-source arbitration (spec.md §1) and the CH445P I2S switch
// it drives. Policy is "last source to start playing wins": a newly
// playing source takes the DAC, and the one it displaced is told to stop
// (this MCU's sources through the preempted callback, A2DP via a CMD_PAUSE
// frame to the ESP32).

typedef enum {
    AUDIO_SOURCE_ESP32,    // A2DP on the wireless MCU; also the idle default
    AUDIO_SOURCE_AIRPLAY,  // RAOP on this MCU
    AUDIO_SOURCE_LOCAL,    // synthesized ambience on this MCU
    AUDIO_SOURCE_RADIO,    // internet radio on this MCU
} audio_source_t;

// Drives GPIO0 (call only after boot -- it is a strap pin) and starts
// watching the ESP32's UART-reported playback state. on_preempted(source)
// runs when one of this MCU's sources loses the DAC to another starting:
// from a timer task when A2DP starts, or from the claiming caller's task.
// It must stop writing PCM before returning; it may wait for the current chunk.
void audio_source_init(void (*on_preempted)(audio_source_t source));

// `source` starts playing and takes the DAC. Claiming AUDIO_SOURCE_ESP32
// hands it back to A2DP (a manual pick).
bool audio_source_claim(audio_source_t source);
// `source` stopped; the DAC goes back to the ESP32 side if it had it.
void audio_source_release(audio_source_t source);

audio_source_t audio_source_active(void);

void audio_source_set_local_preempted(void (*on_preempted)(void));
bool audio_source_claim_local(void);
void audio_source_release_local(void);

// Hold all sources silent during firmware uploads and installation.
void audio_source_suspend(bool suspended);
bool audio_source_suspended(void);


// While held, the switch stays on the S3 side whatever the arbitration
// above decides (it carries on underneath); releasing puts the owner's side
// back. For the alarm chime, which mutes A2DP rather than pausing it.
void audio_source_hold_s3(bool hold);
