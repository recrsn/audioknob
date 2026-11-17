#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "ui/ui.h"

void talk_init(void);       // after NVS and the audio-source arbiter
void talk_network_up(void); // existing mDNS instance, does not initialize it
void talk_rename(void);
void talk_get_state(ui_talk_t *out);
// Nonblocking UI commands. peer is the discovered peer's IPv4 ID; 0 hangs
// up whatever call is ringing or running.
void talk_call(uint32_t peer);
void talk_answer(bool accept);
void talk_set_volume(uint8_t volume);
// Stops writes before returning; safe from another source's claiming task.
void talk_preempt(void);
