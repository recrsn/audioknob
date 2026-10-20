#include "lcd_driver.h"

#include "lcd_init_cmds.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_st77916.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

// Pins from spec.md §7 (ESP32-S3R8 confirmed nets). QSPI has no separate
// D/C line -- the ST77916 vendor driver encodes command vs. data into the
// 32-bit command phase instead, same as the rest of this display family
// (sh8601/axs15231b) that share Waveshare's QSPI round-panel boards.
#define PIN_LCD_QSPI_SCL GPIO_NUM_13
#define PIN_LCD_QSPI_CS  GPIO_NUM_14
#define PIN_LCD_QSPI_D0  GPIO_NUM_15
#define PIN_LCD_QSPI_D1  GPIO_NUM_16
#define PIN_LCD_QSPI_D2  GPIO_NUM_17
#define PIN_LCD_QSPI_D3  GPIO_NUM_18
#define PIN_LCD_RST      GPIO_NUM_21
#define PIN_LCD_BLK      GPIO_NUM_47

#define PIN_TP_INT GPIO_NUM_9
#define PIN_TP_RST GPIO_NUM_10
#define PIN_TP_SDA GPIO_NUM_11 // shared with HAPTIC_SDA
#define PIN_TP_SCL GPIO_NUM_12 // shared with HAPTIC_SCL

#define LCD_H_RES     360
#define LCD_V_RES     360
#define LCD_SPI_HOST  SPI2_HOST
#define LCD_BITS_PER_PIXEL 16

// One partial-redraw buffer, a sixth of the screen, in internal DMA RAM like
// Waveshare's own firmware. A second one would let rendering overlap the
// flush DMA (page swipes measured 37 -> 45 fps) but costs ~43 KB of internal
// RAM that AirPlay's session setup needs.
#define LVGL_DRAW_BUF_LINES 60

// lv_timer_handler()'s "next due" hint, clamped: the floor yields to
// lower-priority tasks, the ceiling keeps touch polling responsive.
#define LVGL_TASK_MIN_DELAY_MS 2
#define LVGL_TASK_MAX_DELAY_MS 10

// Core 1, away from WiFi/lwIP on core 0, and above the knob and UART tasks
// so input-driven redraws aren't held up. Kept below AirPlay's RTP (8) and
// player (15) tasks on the same core: a full-screen render takes ~27 ms,
// and audio must never wait on it.
#define LVGL_TASK_STACK 8192
#define LVGL_TASK_PRIO 7
#define LVGL_TASK_CORE 1

// Waveshare's PWM: 25 kHz is above audible coil whine; 7 bits is plenty.
#define BLK_LEDC_TIMER    LEDC_TIMER_0
#define BLK_LEDC_CHANNEL  LEDC_CHANNEL_0
#define BLK_LEDC_FREQ_HZ  25000
#define BLK_LEDC_RES      LEDC_TIMER_7_BIT
#define BLK_MIN_PCT       5

static const char *TAG = "lcd_driver";
static SemaphoreHandle_t s_lvgl_mutex;
static SemaphoreHandle_t s_flush_done;
static lv_indev_t *s_touch_indev;
static esp_lcd_touch_handle_t s_touch_handle;
static i2c_master_bus_handle_t s_i2c_bus;
static uint8_t s_brightness = CONFIG_AK_BACKLIGHT_DEFAULT_PCT;
static bool s_backlight_on;
static bool s_starting = true;

void lcd_driver_lvgl_lock(void) {
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
}

void lcd_driver_lvgl_unlock(void) {
    xSemaphoreGive(s_lvgl_mutex);
}

static bool notify_flush_ready(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    (void)io;
    (void)edata;
    (void)user_ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    return woken == pdTRUE;
}

// Without this LVGL busy-spins on the flushing flag through every DMA
// transfer, which starved IDLE1 into the task watchdog while AirPlay played.
// LVGL clears the flag itself once this returns.
static void lvgl_flush_wait_cb(lv_display_t *disp) {
    (void)disp;
    xSemaphoreTake(s_flush_done, portMAX_DELAY);
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
    // During startup, finish every strip before returning so the first UI
    // refresh is fully on the panel before its backlight is enabled.
    if (s_starting) {
        lvgl_flush_wait_cb(disp);
        lv_display_flush_ready(disp);
    }
}

// The ST77916 only accepts column/row windows that start on an even and end
// on an odd coordinate; anything else draws shifted, garbled rows.
static void lvgl_rounder_cb(lv_event_t *e) {
    lv_area_t *area = lv_event_get_param(e);
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static void lvgl_tick_cb(void *arg) {
    (void)arg;
    lv_tick_inc(2);
}

#if CONFIG_AK_LVGL_PERF_LOG
// LVGL has no public way to enumerate screens; this debug mode reads them.
#include "lvgl_private.h"

// Each screen gets one full window, then one per visible top-level child
// with that child hidden, so the log shows what each part costs to draw.
#define PERF_FULL_WINDOW_US  (3 * 1000 * 1000)
#define PERF_CHILD_WINDOW_US (2 * 1000 * 1000)

static int64_t s_refr_start_us;
static int64_t s_perf_window_start_us;
static uint32_t s_perf_frames;
static int64_t s_perf_total_us;
static int64_t s_perf_max_us;
static int s_perf_child = -1; // -1: whole screen, else the hidden child
static double s_perf_full_ms;

static int perf_screen_index(lv_display_t *disp) {
    for (uint32_t i = 0; i < disp->screen_cnt; i++) {
        if (disp->screens[i] == lv_screen_active()) {
            return (int)i;
        }
    }
    return -1;
}

// Index 0 is LVGL's default screen, which the UI never shows.
static void perf_next_screen(lv_display_t *disp) {
    int next = perf_screen_index(disp) + 1;
    if (next <= 0 || (uint32_t)next >= disp->screen_cnt) {
        next = 1;
    }
    lv_screen_load(disp->screens[next]);
}

// Hides the next visible child after s_perf_child; false once none are left.
static bool perf_hide_next_child(lv_obj_t *screen) {
    int count = (int)lv_obj_get_child_count(screen);
    for (int i = s_perf_child + 1; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(screen, i);
        if (!lv_obj_is_hidden(child)) {
            s_perf_child = i;
            lv_obj_set_hidden(child, true);
            return true;
        }
    }
    s_perf_child = -1;
    return false;
}

static void perf_log_window(lv_display_t *disp, double avg_ms) {
    lv_obj_t *screen = lv_screen_active();
    int index = perf_screen_index(disp);
    if (s_perf_child < 0) {
        s_perf_full_ms = avg_ms;
        ESP_LOGI(TAG, "perf: screen %d: %.1f ms/frame (%.1f max), internal free %u KB (min %u KB)", index, avg_ms,
                 s_perf_max_us / 1000.0, heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
                 heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024);
        return;
    }
    lv_obj_t *child = lv_obj_get_child(screen, s_perf_child);
    lv_area_t a;
    lv_obj_get_coords(child, &a);
    ESP_LOGI(TAG, "perf: screen %d child %d %s %dx%d: costs %.1f ms", index, s_perf_child,
             lv_obj_get_class(child)->name ? lv_obj_get_class(child)->name : "?", (int)lv_area_get_width(&a),
             (int)lv_area_get_height(&a), s_perf_full_ms - avg_ms);
    lv_obj_set_hidden(child, false);
}

static void perf_refr_cb(lv_event_t *e) {
    int64_t now = esp_timer_get_time();
    if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
        s_refr_start_us = now;
        return;
    }
    int64_t frame_us = now - s_refr_start_us;
    s_perf_frames++;
    s_perf_total_us += frame_us;
    if (frame_us > s_perf_max_us) {
        s_perf_max_us = frame_us;
    }
    int64_t window_us = s_perf_child < 0 ? PERF_FULL_WINDOW_US : PERF_CHILD_WINDOW_US;
    if (now - s_perf_window_start_us < window_us) {
        return;
    }

    lv_display_t *disp = lv_event_get_target(e);
    perf_log_window(disp, s_perf_total_us / 1000.0 / (s_perf_frames ? s_perf_frames : 1));
    if (!perf_hide_next_child(lv_screen_active())) {
        perf_next_screen(disp);
    }
    // Idle app pages go home on their own; this keeps each one up.
    lv_display_trigger_activity(disp);
    s_perf_window_start_us = now;
    s_perf_frames = 0;
    s_perf_total_us = 0;
    s_perf_max_us = 0;
}

static void perf_init(lv_display_t *disp) {
    lv_display_add_event_cb(disp, perf_refr_cb, LV_EVENT_REFR_START, NULL);
    lv_display_add_event_cb(disp, perf_refr_cb, LV_EVENT_REFR_READY, NULL);
    s_perf_window_start_us = esp_timer_get_time();
}
#endif

static void lvgl_task(void *arg) {
    (void)arg;
    while (1) {
        lcd_driver_lvgl_lock();
#if CONFIG_AK_LVGL_PERF_LOG
        lv_obj_invalidate(lv_screen_active());
#endif
        uint32_t idle_ms = lv_timer_handler();
        lcd_driver_lvgl_unlock();
        if (idle_ms < LVGL_TASK_MIN_DELAY_MS) {
            idle_ms = LVGL_TASK_MIN_DELAY_MS;
        } else if (idle_ms > LVGL_TASK_MAX_DELAY_MS) {
            idle_ms = LVGL_TASK_MAX_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(idle_ms));
    }
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    (void)indev;
    esp_lcd_touch_read_data(s_touch_handle);

    uint16_t x, y;
    uint8_t count = 0;
    bool pressed = esp_lcd_touch_get_coordinates(s_touch_handle, &x, &y, NULL, &count, 1);

    if (pressed && count > 0) {
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static lv_display_t *lvgl_display_init(esp_lcd_panel_handle_t panel_handle, esp_lcd_panel_io_handle_t io_handle) {
    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_user_data(disp, panel_handle);
    // The panel takes big-endian RGB565; rendering it directly saves swapping
    // every flushed pixel on the CPU.
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);
    s_flush_done = xSemaphoreCreateBinary();
    ESP_ERROR_CHECK(s_flush_done ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_flush_wait_cb(disp, lvgl_flush_wait_cb);

    size_t buf_px = (size_t)LCD_H_RES * LVGL_DRAW_BUF_LINES;
    void *buf = heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(buf ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_buffers(disp, buf, NULL, buf_px * sizeof(uint16_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, lvgl_rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = notify_flush_ready};
    esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, disp);

    return disp;
}

static void backlight_apply(void) {
    uint32_t duty = s_backlight_on ? (uint32_t)s_brightness * ((1u << BLK_LEDC_RES) - 1) / 100 : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BLK_LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BLK_LEDC_CHANNEL);
}

void lcd_driver_set_brightness(uint8_t pct) {
    if (pct > 100) {
        pct = 100;
    } else if (pct < BLK_MIN_PCT) {
        pct = BLK_MIN_PCT;
    }
    s_brightness = pct;
    backlight_apply();
}

void lcd_driver_set_backlight_on(bool on) {
    s_backlight_on = on;
    backlight_apply();
}

void lcd_driver_backlight_init(void) {
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BLK_LEDC_RES,
        .timer_num = BLK_LEDC_TIMER,
        .freq_hz = BLK_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));
    const ledc_channel_config_t chan_cfg = {
        .gpio_num = PIN_LCD_BLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BLK_LEDC_CHANNEL,
        .timer_sel = BLK_LEDC_TIMER,
        .duty = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&chan_cfg));
    backlight_apply();
}

void lcd_driver_show_ui(void) {
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(NULL);
    s_starting = false;
    lcd_driver_set_backlight_on(true);
}

static i2c_master_bus_handle_t touch_i2c_bus_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = PIN_TP_SDA,
        .scl_io_num = PIN_TP_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    return bus;
}

static void touch_init(lv_display_t *disp) {
    s_i2c_bus = touch_i2c_bus_init();

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io_handle));

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = PIN_TP_INT,
        .levels = {.reset = 0, .interrupt = 0},
        // Matches the panel's 180° rotation in lcd_driver_init().
        .flags = {.swap_xy = 0, .mirror_x = 1, .mirror_y = 1},
    };
    ESP_ERROR_CHECK(esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &s_touch_handle));

    s_touch_indev = lv_indev_create();
    lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(s_touch_indev, disp);
    lv_indev_set_read_cb(s_touch_indev, touch_read_cb);
}

void lcd_driver_init(void) {
    s_lvgl_mutex = xSemaphoreCreateMutex();
    lv_init();

    const spi_bus_config_t bus_cfg = ST77916_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_QSPI_SCL, PIN_LCD_QSPI_D0, PIN_LCD_QSPI_D1, PIN_LCD_QSPI_D2, PIN_LCD_QSPI_D3,
        LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(PIN_LCD_QSPI_CS, NULL, NULL);
    // Waveshare's own firmware runs this panel at 80 MHz (a full frame in
    // ~7 ms). We once saw pixel noise at 40 MHz; if it comes back, drop
    // CONFIG_AK_LCD_PCLK_MHZ to 20.
    io_cfg.pclk_hz = CONFIG_AK_LCD_PCLK_MHZ * 1000 * 1000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io_handle));

    const st77916_vendor_config_t vendor_cfg = {
        .init_cmds = waveshare_knob_lcd_init_cmds,
        .init_cmds_size = sizeof(waveshare_knob_lcd_init_cmds) / sizeof(waveshare_knob_lcd_init_cmds[0]),
        .flags = {.use_qspi_interface = 1},
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .vendor_config = (void *)&vendor_cfg,
    };
    esp_lcd_panel_handle_t panel_handle = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_st77916(io_handle, &panel_cfg, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    // The panel is mounted upside down relative to the knob's natural
    // orientation; mirroring both axes in MADCTL rotates it 180° for free.
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    lv_display_t *disp = lvgl_display_init(panel_handle, io_handle);
#if CONFIG_AK_LVGL_PERF_LOG
    perf_init(disp);
#endif

    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2 * 1000));

    touch_init(disp);

    xTaskCreatePinnedToCore(lvgl_task, "lvgl", LVGL_TASK_STACK, NULL, LVGL_TASK_PRIO, NULL, LVGL_TASK_CORE);

    ESP_LOGI(TAG, "LCD + touch initialized (%dx%d, %d MHz QSPI)", LCD_H_RES, LCD_V_RES, CONFIG_AK_LCD_PCLK_MHZ);
}

i2c_master_bus_handle_t lcd_driver_i2c_bus(void) {
    return s_i2c_bus;
}

lv_indev_t *lcd_driver_touch_indev(void) {
    return s_touch_indev;
}
