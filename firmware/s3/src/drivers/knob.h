#pragma once

// This MCU's encoder (EC1_A/EC1_B) and its push button SW2, which is wired
// as a short-to-GND across both encoder lines (no separate GPIO). Captures
// GPIO edge interrupts; an LVGL encoder consumes them. Callbacks run on
// the LVGL task with its lock held. Call knob_start with that lock held.
// Whether a turn/press means anything is the caller's call, per the
// control-ownership rule.

typedef void (*knob_rotate_cb_t)(int steps);
typedef void (*knob_press_cb_t)(void);

// on_press fires on release of a short press; on_long_press once a press is
// held past 500 ms, and that press then fires nothing on release.
void knob_start(knob_rotate_cb_t on_rotate, knob_press_cb_t on_press, knob_press_cb_t on_long_press);
