#pragma once

#include <stdbool.h>
#include <stdint.h>

// The volume HUD: one ring just inside the edge, over every screen on
// lv_layer_top(). It appears only when a playing source's volume moves,
// then fades; between flashes it is hidden, so updating it costs nothing.

void volume_ring_build(void);
// The source in front, its volume and whether it is connected. A source
// that has just connected (or taken over) settles silently: its first
// volumes come from the sender, not from anyone turning it.
void volume_ring_update(int source, bool connected, uint8_t volume);
void volume_ring_set_tint(uint32_t tint);
