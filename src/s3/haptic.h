#pragma once

#include "driver/i2c_master.h"

// DRV2605(L) + LRA on the touch I2C bus. Its EN/TRIG nets aren't on any S3
// GPIO, so the chip is driven purely over I2C (internal-trigger mode). If it
// doesn't answer the probe, every call below is a no-op.
void haptic_init(i2c_master_bus_handle_t bus);

// Repeating buzz for a ringing timer, until haptic_alarm_stop().
void haptic_alarm_start(void);
void haptic_alarm_stop(void);
