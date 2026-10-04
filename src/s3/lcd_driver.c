#include "lcd_driver.h"

#include "lcd_init_cmds.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
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

// Partial-redraw buffers, a ninth of the screen each, in internal DMA RAM
// like Waveshare's own demo: the QSPI DMA reads them directly.
#define LVGL_DRAW_BUF_LINES 40

static const char *TAG = "lcd_driver";
static SemaphoreHandle_t s_lvgl_mutex;
static lv_indev_t *s_touch_indev;
static esp_lcd_touch_handle_t s_touch_handle;

void lcd_driver_lvgl_lock(void) {
    xSemaphoreTake(s_lvgl_mutex, portMAX_DELAY);
}

void lcd_driver_lvgl_unlock(void) {
    xSemaphoreGive(s_lvgl_mutex);
}

static bool notify_flush_ready(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
    (void)io;
    (void)edata;
    lv_display_t *disp = (lv_display_t *)user_ctx;
    lv_display_flush_ready(disp);
    return false;
}

static void lvgl_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    // LVGL renders little-endian RGB565; the panel takes it big-endian.
    lv_draw_sw_rgb565_swap(px_map, lv_area_get_size(area));
    esp_lcd_panel_draw_bitmap(panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px_map);
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

static void lvgl_task(void *arg) {
    (void)arg;
    while (1) {
        lcd_driver_lvgl_lock();
        uint32_t idle_ms = lv_timer_handler();
        lcd_driver_lvgl_unlock();
        vTaskDelay(pdMS_TO_TICKS(idle_ms ? idle_ms : 1));
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
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, lvgl_flush_cb);

    size_t buf_px = (size_t)LCD_H_RES * LVGL_DRAW_BUF_LINES;
    void *buf1 = heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    void *buf2 = heap_caps_malloc(buf_px * sizeof(uint16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    ESP_ERROR_CHECK(buf1 && buf2 ? ESP_OK : ESP_ERR_NO_MEM);
    lv_display_set_buffers(disp, buf1, buf2, buf_px * sizeof(uint16_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, lvgl_rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    const esp_lcd_panel_io_callbacks_t cbs = {.on_color_trans_done = notify_flush_ready};
    esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, disp);

    return disp;
}

static void backlight_init(void) {
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_LCD_BLK,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    gpio_set_level(PIN_LCD_BLK, 1);
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
    i2c_master_bus_handle_t i2c_bus = touch_i2c_bus_init();

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_cfg, &tp_io_handle));

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = LCD_H_RES,
        .y_max = LCD_V_RES,
        .rst_gpio_num = PIN_TP_RST,
        .int_gpio_num = PIN_TP_INT,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
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

    backlight_init();

    const spi_bus_config_t bus_cfg = ST77916_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_QSPI_SCL, PIN_LCD_QSPI_D0, PIN_LCD_QSPI_D1, PIN_LCD_QSPI_D2, PIN_LCD_QSPI_D3,
        LCD_H_RES * LVGL_DRAW_BUF_LINES * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = ST77916_PANEL_IO_QSPI_CONFIG(PIN_LCD_QSPI_CS, NULL, NULL);
    // The driver's 40 MHz default shows pixel noise on this board; 20 MHz
    // (next clean divider of the 80 MHz APB) still moves a full frame in ~26 ms.
    io_cfg.pclk_hz = 20 * 1000 * 1000;
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
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    lv_display_t *disp = lvgl_display_init(panel_handle, io_handle);

    const esp_timer_create_args_t tick_timer_args = {
        .callback = lvgl_tick_cb,
        .name = "lv_tick",
    };
    esp_timer_handle_t tick_timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, 2 * 1000));

    touch_init(disp);

    xTaskCreate(lvgl_task, "lvgl", 8192, NULL, 4, NULL);

    ESP_LOGI(TAG, "LCD + touch initialized (%dx%d)", LCD_H_RES, LCD_V_RES);
}
