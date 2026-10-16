#pragma once

#include <stdint.h>

// The ringing countdown's chime through the DAC, synthesized (a few decaying
// sine partials per note) rather than stored. While it sounds it holds the
// S3 side of the CH445P switch, so a Bluetooth stream is muted rather than
// paused, and AirPlay is ducked under it; both come back when it stops.

typedef enum {
    ALARM_SOUND_TIMER,     // ding-dong
    ALARM_SOUND_FOCUS_END, // falling: time for a break
    ALARM_SOUND_BREAK_END, // rising: back to work
} alarm_sound_kind_t;

typedef enum {
    ALARM_SOUND_CHIME, // soft, mostly harmonic
    ALARM_SOUND_BELL,  // brighter, inharmonic, longer ring
} alarm_sound_voice_t;

// After audio_out_init() and audio_source_init().
void alarm_sound_init(void);

// Takes effect from the next note; volume_pct is cubic like AirPlay's.
void alarm_sound_configure(alarm_sound_voice_t voice, uint8_t volume_pct);

// Repeats until alarm_sound_stop(); a new kind replaces a ringing one. Never
// blocks.
void alarm_sound_start(alarm_sound_kind_t kind);
void alarm_sound_stop(void);

// Plays kind's pattern once, as a sample of the current settings; ignored
// while an alarm rings.
void alarm_sound_preview(alarm_sound_kind_t kind);
