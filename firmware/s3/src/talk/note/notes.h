#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui/ui.h"

// The threads: voice notes and calls per knob, kept on flash (store.c) and
// in PSRAM. Every flash access and every send runs on one worker task with an
// internal stack, created while there's work; the rest only queues it, so
// any task may call these.

// Ogg Opus, one 20 ms packet a page, 60 s at most.
#define NOTE_MAX_BYTES (320 * 1024)

// Mounts the store and loads the threads, in the background.
void notes_init(const char *own_key);
// The knobs discovery found, which threads can reach.
void notes_set_peers(const ui_talk_peer_t *peers, int count);

uint32_t notes_version(void);
void notes_snapshot(ui_notes_t *out);

// Takes ogg (PSRAM): saves it as the newest note to `key` and sends it.
void notes_send(const char *key, uint8_t *ogg, size_t len, uint16_t tenths, const uint8_t wave[UI_NOTE_WAVE]);
void notes_resend(const char *id);
// Loads the note and hands it to note_audio_play().
void notes_play(const char *id);
// The note played to its end: an incoming one is marked played, and its
// sender told.
void notes_played(const char *id);
void notes_remove(const char *id);
void notes_seen(const char *key);
void notes_log_call(const char *key, const char *name, bool incoming, ui_call_outcome_t outcome, uint32_t seconds);

// From the RTSP connection task. Takes ogg; returns the reply's status.
int notes_receive(const char *key, const char *name, const char *id, const uint8_t wave[UI_NOTE_WAVE], uint8_t *ogg,
                  size_t len);
void notes_receipt(const char *key, const char *id);

bool notes_valid_key(const char *key);
bool notes_valid_id(const char *id);
