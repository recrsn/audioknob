#pragma once

// This MCU's encoder (EC1_A/EC1_B) and its push button SW2, which is wired
// as a short-to-GND across both encoder lines (no separate GPIO). Starts a
// polling task; callbacks run on it, so they must not block for long.
// Whether a turn/press means anything is the caller's call, per the
// control-ownership rule.

typedef void (*knob_rotate_cb_t)(int steps);
typedef void (*knob_press_cb_t)(void);

void knob_start(knob_rotate_cb_t on_rotate, knob_press_cb_t on_press);
