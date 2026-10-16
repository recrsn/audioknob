#pragma once

#include "driver/i2c_master.h"

// DRV2605(L) + LRA on the touch I2C bus. Its EN/TRIG nets aren't on any S3
// GPIO, so the chip is driven purely over I2C (internal-trigger mode). If it
// doesn't answer the probe, every call below is a no-op.
void haptic_init(i2c_master_bus_handle_t bus);

typedef enum {
    HAPTIC_TICK,   // a knob detent that did something
    HAPTIC_CLICK,  // a knob press or a tap
    HAPTIC_LONG,   // a knob long press
} haptic_effect_t;

// One-shot feedback; a newer effect cuts off one still playing. Dropped
// while the alarm is buzzing so it can't swallow the alarm.
void haptic_play(haptic_effect_t effect);

// Repeating buzz for a ringing timer, until haptic_alarm_stop().
void haptic_alarm_start(void);
void haptic_alarm_stop(void);
