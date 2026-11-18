#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui/ui.h"

// Recording and playing voice notes. The requests come from any task; the
// work runs a 20 ms frame at a time on the Talk worker (note_audio_step()),
// whose call stack Opus needs. Playback rides on alarm_sound's clip, so
// whatever plays ducks under a note and comes back after it; recording
// ducks it too.

void note_audio_init(void);

// Whether the worker has frames to run.
bool note_audio_busy(void);
// Recording right now: calls are turned away meanwhile.
bool note_audio_recording(void);
// One frame, at the Talk volume.
void note_audio_step(uint8_t volume);

// A new note to `key`, replacing any draft.
void note_audio_record(const char *key);
// Stops a recording and sends it, or sends the draft.
void note_audio_send(void);
void note_audio_discard(void);
// Stops a recording, keeping it as the draft.
void note_audio_pause(void);

void note_audio_play_request(const char *id);
// From notes.c, once loaded. Takes ogg.
void note_audio_play(const char *id, uint8_t *ogg, size_t len);
void note_audio_stop(void);

// The rec* / level / playing / play_tenths fields.
void note_audio_get(ui_talk_t *out);
