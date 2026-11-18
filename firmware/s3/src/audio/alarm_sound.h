#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The ringing countdown's chime through the DAC, synthesized (a few decaying
// sine partials per note) rather than stored. While it sounds it holds the
// S3 side of the CH445P switch, so a Bluetooth stream is muted rather than
// paused, and AirPlay is ducked under it; both come back when it stops.

typedef enum {
    ALARM_SOUND_TIMER,     // ding-dong
    ALARM_SOUND_FOCUS_END, // falling: time for a break
    ALARM_SOUND_BREAK_END, // rising: back to work
    ALARM_SOUND_CALL,      // a Talk call ringing
    ALARM_SOUND_NOTE,      // a voice note arrived (alarm_sound_notify())
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
// waits for queue space or playback. Commands briefly serialize on a mutex;
// pending replacements coalesce, dropping the oldest if the 8-slot queue fills.
// The system-sound worker and its playback LUT exist only while needed.
// Allocation failure logs a warning and cancels pending requests; a later call
// can retry.
void alarm_sound_start(alarm_sound_kind_t kind);
void alarm_sound_stop(void);

// Plays kind's pattern once, as a sample of the current settings; ignored
// while an alarm rings.
void alarm_sound_preview(alarm_sound_kind_t kind);
// Plays kind once as a notification, at its own fixed level.
void alarm_sound_notify(alarm_sound_kind_t kind);

// A clip (a Talk voice note) over whatever plays, which ducks under it like
// under the chime: 16 kHz mono PCM, already at its volume, written while it
// plays. While on, the clip ducks playback even with nothing written (as
// while a note is recorded). One writer at a time.
bool alarm_sound_clip_start(void);
// Waits up to wait_ms for room; returns the frames queued.
size_t alarm_sound_clip_write(const int16_t *pcm, size_t frames, uint32_t wait_ms);
// Nothing more to write: it plays out what's queued.
void alarm_sound_clip_finish(void);
// Until a finished clip has played out.
bool alarm_sound_clip_playing(void);
// Silences it at once, dropping what's queued, and lifts the duck.
void alarm_sound_clip_stop(void);
