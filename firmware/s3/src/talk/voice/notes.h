#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ui/ui.h"
void voice_notes_init(void);
bool voice_notes_record(uint32_t peer, char id[17]); // peer=0 saves locally
void voice_notes_stop(void);
bool voice_notes_play(const char *id);
void voice_notes_preempt(void);
void voice_notes_get_state(ui_voice_t *out);
bool voice_notes_receive(uint32_t peer, const char *sender, const char *id, const void *ogg, size_t length);
void *voice_notes_read(const char *id, size_t *length);
