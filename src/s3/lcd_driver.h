#pragma once

#include <stdint.h>

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
