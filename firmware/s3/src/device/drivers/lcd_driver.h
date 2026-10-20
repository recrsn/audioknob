#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "lvgl.h"

// Configure the backlight off at the start of app_main(), before other
// peripherals initialize. Brightness changes stay dark until show_ui().
void lcd_driver_backlight_init(void);

// Brings up the ST77916 QSPI LCD and CST816 touch (see spec.md
// §7 for the pin table this is wired against), and registers both with
// LVGL. Must run before ui_init() or any other LVGL call.
void lcd_driver_init(void);

// Ask the LVGL task to render the first UI frame, then light the screen
// after the transfer completes.
// Call once after UI/settings initialization, with the LVGL lock held.
void lcd_driver_show_ui(void);

// Backlight PWM duty in percent, clamped to 5..100 (0 would look like a
// dead board). Starts at CONFIG_AK_BACKLIGHT_DEFAULT_PCT.
void lcd_driver_set_brightness(uint8_t pct);

// Switches the backlight fully off for display sleep and back on at the set
// brightness; brightness changes while off apply on waking.
void lcd_driver_set_backlight_on(bool on);

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
