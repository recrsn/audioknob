#include "settings_ui.h"

#include <stdio.h>
#include <string.h>

#include "airplay.h"
#include "device_name.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lcd_driver.h"
#include "lvgl.h"
#include "online.h"
#include "settings.h"
#include "tz.h"
#include "uart_link.h"
#include "ui.h"
#include "wifi.h"

#define REFRESH_MS     1000
// Knob turns change brightness in many small steps; write flash once they
// settle.
#define SAVE_DELAY_US  (2 * 1000 * 1000)

static const char *TAG = "settings_ui";

static settings_t s_settings;
static char s_zone[40]; // zone in effect
static uint32_t s_geo_version;
static esp_timer_handle_t s_save_timer;

// Runs on the esp_timer task; s_settings belongs to the LVGL task.
static void save_cb(void *arg) {
    (void)arg;
    lcd_driver_lvgl_lock();
    settings_t copy = s_settings;
    lcd_driver_lvgl_unlock();
    settings_save(&copy);
}

static void save_later(void) {
    esp_timer_stop(s_save_timer);
    esp_timer_start_once(s_save_timer, SAVE_DELAY_US);
}

// A manual zone wins; otherwise the location's (manual or IP-detected), by name when tz_db
// knows it, else by its current offset; UTC until that's known.
static void apply_timezone(void) {
    if (s_settings.tz[0] && tz_apply(s_settings.tz)) {
        snprintf(s_zone, sizeof(s_zone), "%s", s_settings.tz);
        return;
    }
    geo_t geo;
    if (!online_get_location(&geo, NULL)) {
        tz_apply("UTC");
        s_zone[0] = '\0';
        return;
    }
    if (tz_apply(geo.tz)) {
        snprintf(s_zone, sizeof(s_zone), "%s", geo.tz);
    } else if (geo.has_offset) {
        tz_apply_offset(geo.tz_offset_s);
        snprintf(s_zone, sizeof(s_zone), "%s", geo.tz);
        ESP_LOGW(TAG, "%s not in tz_db; using a fixed offset", geo.tz);
    }
}

static void apply_airplay(void) {
    airplay_set_protocols(s_settings.airplay & UI_AIRPLAY_1, s_settings.airplay & UI_AIRPLAY_2);
}

static void fill(ui_settings_t *out) {
    *out = (ui_settings_t){
        .brightness = s_settings.brightness,
        .sleep_s = s_settings.sleep_s,
        .clock_24h = s_settings.clock_24h,
        .fahrenheit = s_settings.fahrenheit,
        .haptics = s_settings.haptics,
        .bluetooth = s_settings.bluetooth,
        .airplay = s_settings.airplay,
        .airplay2_available = airplay_supports_airplay2(),
        .tz_auto = !s_settings.tz[0],
        .free_internal_kb = heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
        .free_psram_kb = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024,
    };
    snprintf(out->tz_name, sizeof(out->tz_name), "%s", s_zone);
    geo_t geo;
    bool manual = false;
    if (online_get_location(&geo, &manual)) {
        snprintf(out->location, sizeof(out->location), "%s", geo.city);
    }
    out->location_auto = !manual;
    device_name_get(out->device_name, sizeof(out->device_name));
    device_name_hostname(out->hostname, sizeof(out->hostname));
    snprintf(out->version, sizeof(out->version), "%s", esp_app_get_description()->version);
    wifi_get_station(out->wifi_ssid, sizeof(out->wifi_ssid), out->ip, sizeof(out->ip));
}

static void push(void) {
    ui_settings_t ui;
    fill(&ui);
    ui_update_settings(&ui);
}

static void on_setting(ui_setting_t setting, int value, const char *zone) {
    switch (setting) {
    case UI_SETTING_BRIGHTNESS:
        s_settings.brightness = (uint8_t)value;
        lcd_driver_set_brightness(s_settings.brightness);
        break;
    case UI_SETTING_SLEEP:
        s_settings.sleep_s = (uint16_t)value;
        break;
    case UI_SETTING_CLOCK_24H:
        s_settings.clock_24h = value;
        break;
    case UI_SETTING_FAHRENHEIT:
        s_settings.fahrenheit = value;
        break;
    case UI_SETTING_HAPTICS:
        s_settings.haptics = value;
        break;
    case UI_SETTING_TIMEZONE:
        snprintf(s_settings.tz, sizeof(s_settings.tz), "%s", zone ? zone : "");
        apply_timezone();
        break;
    case UI_SETTING_BLUETOOTH:
        s_settings.bluetooth = value;
        uart_link_set_bluetooth(s_settings.bluetooth);
        break;
    case UI_SETTING_AIRPLAY:
        s_settings.airplay = (uint8_t)value;
        apply_airplay();
        break;
    case UI_SETTING_LOCATION:
        // online.c persists the location itself.
        online_set_location(NULL);
        apply_timezone();
        push();
        return;
    case UI_SETTING_FORGET_WIFI:
        wifi_forget();
        return;
    }
    save_later();
    push();
}

static void refresh_cb(lv_timer_t *timer) {
    (void)timer;
    geo_t geo;
    uint32_t v = online_get_location(&geo, NULL);
    if (v != s_geo_version) {
        s_geo_version = v;
        apply_timezone();
    }
    push();
}

void settings_ui_init(void) {
    settings_load(&s_settings);
    lcd_driver_set_brightness(s_settings.brightness);
    apply_timezone();
    uart_link_set_bluetooth(s_settings.bluetooth);
    apply_airplay();
    const esp_timer_create_args_t args = {.callback = save_cb, .name = "settings_save"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_save_timer));
}

void settings_ui_start(void) {
    ui_set_settings_handler(on_setting);
    push();
    lv_timer_create(refresh_cb, REFRESH_MS, NULL);
}

void settings_ui_snapshot(ui_settings_t *out) {
    lcd_driver_lvgl_lock();
    fill(out);
    lcd_driver_lvgl_unlock();
}

void settings_ui_change(ui_setting_t setting, int value, const char *zone) {
    lcd_driver_lvgl_lock();
    on_setting(setting, value, zone);
    lcd_driver_lvgl_unlock();
}

void settings_ui_set_location(const geo_t *geo) {
    lcd_driver_lvgl_lock();
    online_set_location(geo);
    apply_timezone();
    push();
    lcd_driver_lvgl_unlock();
}

bool settings_ui_haptics(void) {
    return s_settings.haptics;
}
