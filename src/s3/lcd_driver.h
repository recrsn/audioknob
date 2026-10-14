#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "lvgl.h"

// Brings up the ST77916 QSPI LCD, CST816 touch, and backlight (see spec.md
// §7 for the pin table this is wired against), and registers both with
// LVGL. Must run before ui_init() or any other LVGL call.
void lcd_driver_init(void);

// Backlight PWM duty in percent, clamped to 5..100 (0 would look like a
// dead board). Starts at CONFIG_AK_BACKLIGHT_DEFAULT_PCT.
void lcd_driver_set_brightness(uint8_t pct);

// lcd_driver_init() starts a task that owns lv_timer_handler(); every LVGL
// call made from anywhere else (ui_init(), ui_update_*() from the UART RX
// task) must be wrapped in these to stay off that task's toes.
void lcd_driver_lvgl_lock(void);
void lcd_driver_lvgl_unlock(void);

// The CST816 touch input device. Valid after lcd_driver_init().
lv_indev_t *lcd_driver_touch_indev(void);

// The touch I2C bus (GPIO11/12), which the DRV2605 haptic driver shares.
// Valid after lcd_driver_init().
i2c_master_bus_handle_t lcd_driver_i2c_bus(void);
