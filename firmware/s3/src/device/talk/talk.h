#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ui.h"

void talk_init(void);       // after NVS and the audio-source arbiter
void talk_network_up(void); // existing mDNS instance, does not initialize it
void talk_rename(void);
void talk_get_state(ui_talk_t *out);
// Nonblocking UI commands. peer is the discovered peer's IPv4 ID.
void talk_press(uint32_t peer, bool held);
void talk_set_volume(uint8_t volume);
// Stops writes before returning; safe from another source's claiming task.
void talk_preempt(void);

// Persists debounced volume on the messages task (an internal flash-safe stack).
void talk_persist_settings(void);
