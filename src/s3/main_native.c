// Desktop entry point for the LVGL native emulator target (env:native).
// Mirrors main.c's role on real hardware, but drives an SDL window instead
// of the QSPI LCD, so the UI in ui.c can be iterated on without flashing.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lights.h"
#include "lvgl.h"
#include "tz.h"
#include "ui.h"

#define WINDOW_WIDTH  360
#define WINDOW_HEIGHT 360

static void clock_tick_cb(lv_timer_t *timer) {
    (void)timer;
    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    ui_update_clock(true, &local_time);
}

// Settings live in memory only. "Automatic" timezone means the host's own
// zone, read from /etc/localtime, since there's no IP lookup here; the
// location starts pinned so going back to automatic can be tried.
static ui_settings_t s_settings = {
    .brightness = 80,
    .sleep_s = 300,
    .clock_24h = true,
    .haptics = true,
    .bluetooth = true,
    .airplay = UI_AIRPLAY_1 | UI_AIRPLAY_2,
    .airplay2_available = true,
    .tz_auto = true,
    .location = "Lisbon",
    .device_name = "AudioKnob-K7MXQ2PA",
    .hostname = "audioknob-k7mxq2pa",
    .wifi_ssid = "Sample",
    .ip = "192.168.1.50",
    .version = "native",
};

static void host_zone(char *out, size_t len) {
    char link[256];
    ssize_t n = readlink("/etc/localtime", link, sizeof(link) - 1);
    out[0] = '\0';
    if (n > 0) {
        link[n] = '\0';
        const char *zone = strstr(link, "zoneinfo/");
        snprintf(out, len, "%s", zone ? zone + strlen("zoneinfo/") : "");
    }
}

static void on_setting(ui_setting_t setting, int value, const char *zone) {
    switch (setting) {
    case UI_SETTING_BRIGHTNESS:
        s_settings.brightness = (uint8_t)value;
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
    case UI_SETTING_SLEEP:
        s_settings.sleep_s = (uint16_t)value;
        break;
    case UI_SETTING_TIMEZONE:
        s_settings.tz_auto = zone == NULL;
        if (zone && tz_apply(zone)) {
            snprintf(s_settings.tz_name, sizeof(s_settings.tz_name), "%s", zone);
        } else {
            unsetenv("TZ");
            tzset();
            host_zone(s_settings.tz_name, sizeof(s_settings.tz_name));
        }
        break;
    case UI_SETTING_LOCATION:
        s_settings.location_auto = true;
        snprintf(s_settings.location, sizeof(s_settings.location), "Sample");
        break;
    case UI_SETTING_FORGET_WIFI:
        printf("settings: forget WiFi (no-op on native)\n");
        break;
    case UI_SETTING_BLUETOOTH:
        s_settings.bluetooth = value;
        break;
    case UI_SETTING_AIRPLAY:
        s_settings.airplay = (uint8_t)value;
        break;
    }
    ui_update_settings(&s_settings);
}

// Stands in for the ESP32's pairing state machine so the prompt can be laid
// out: a "phone" asks to pair 3 s into the window with a fixed code.
#define SIM_WINDOW_S 120
static pairing_status_t s_pairing = {.bonded = 1};
static int s_pairing_hold;

static void pairing_tick_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_pairing_hold && --s_pairing_hold == 0) {
        s_pairing.state = s_pairing.seconds_left ? PAIRING_OPEN : PAIRING_IDLE;
    }
    if (s_pairing.seconds_left && --s_pairing.seconds_left == 0) {
        s_pairing.state = PAIRING_IDLE;
    }
    if (s_pairing.state == PAIRING_OPEN && s_pairing.seconds_left == SIM_WINDOW_S - 3) {
        s_pairing.state = PAIRING_CONFIRM;
        s_pairing.code = 42917;
    }
    if (s_pairing.state == PAIRING_BUSY && !s_pairing_hold) {
        s_pairing.state = PAIRING_DONE;
        s_pairing.seconds_left = 0;
        s_pairing.bonded++;
        snprintf(s_pairing.peer, sizeof(s_pairing.peer), "Sample Phone");
        s_pairing_hold = 3;
    }
    ui_update_pairing(s_settings.bluetooth ? &s_pairing : NULL);
}

static void on_pairing(ui_pairing_action_t action) {
    switch (action) {
    case UI_PAIRING_OPEN:
        s_pairing.state = PAIRING_OPEN;
        s_pairing.seconds_left = SIM_WINDOW_S;
        break;
    case UI_PAIRING_CLOSE:
    case UI_PAIRING_REJECT:
        s_pairing.state = PAIRING_IDLE;
        s_pairing.seconds_left = 0;
        break;
    case UI_PAIRING_ACCEPT:
        s_pairing.state = PAIRING_BUSY;
        break;
    case UI_PAIRING_FORGET:
        s_pairing.bonded = 0;
        break;
    }
    s_pairing.code = s_pairing.state == PAIRING_CONFIRM ? s_pairing.code : 0;
    ui_update_pairing(&s_pairing);
}

// Fixed sample so the Weather page can be laid out without a network; the
// place name says so.
static void show_sample_weather(void) {
    ui_weather_t w = {
        .valid = true,
        .place = "Sample data",
        .temp_c = 27.4f,
        .high_c = 31.0f,
        .low_c = 21.2f,
        .code = 2,
        .is_day = true,
        .hours = {{15, 28.1f, 2, true}, {16, 27.0f, 80, true}, {17, 25.6f, 95, true}, {18, 24.2f, 3, false}},
    };
    ui_update_weather(&w);
}

// Stands in for a source so the now-playing screens can be laid out: P steps
// through nothing, Bluetooth waiting to pair, and AirPlay playing a track,
// paused, then playing again.
static void cycle_sample_playback(void) {
    static const struct {
        wireless_state_t state;
        bool track;
    } k_steps[] = {
        {{.volume = 0, .connected = 0, .playing = 0, .source = SOURCE_AIRPLAY}, false},
        {{.volume = 0, .connected = 0, .playing = 1, .source = SOURCE_A2DP}, false},
        {{.volume = 45, .connected = 1, .playing = 1, .source = SOURCE_AIRPLAY}, true},
        {{.volume = 45, .connected = 1, .playing = 0, .source = SOURCE_AIRPLAY}, true},
    };
    static const ui_track_info_t k_track = {
        .title = "Sample Track Title",
        .artist = "Sample Artist",
        .album = "Sample Album",
    };
    static const ui_track_info_t k_none;
    static size_t step;
    step = (step + 1) % (sizeof(k_steps) / sizeof(k_steps[0]));
    ui_update_wireless(&k_steps[step].state, k_steps[step].track ? &k_track : &k_none);
}

// The panel is round; everything outside its circle is bezel.
static void panel_mask_create(void) {
    lv_obj_t *mask = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(mask);
    lv_obj_remove_flag(mask, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(mask, WINDOW_WIDTH, WINDOW_HEIGHT);
    lv_obj_set_style_radius(mask, LV_RADIUS_CIRCLE, 0);
    // Wide enough to reach the window's corners from the circle's edge.
    lv_obj_set_style_outline_width(mask, WINDOW_WIDTH / 4, 0);
    lv_obj_set_style_outline_color(mask, lv_color_hex(0x2a2a30), 0);
    lv_obj_set_style_outline_opa(mask, LV_OPA_COVER, 0);
    lv_obj_center(mask);
}

// Stands in for the hardware knob: Up/Down arrows turn it, Enter presses it
// and holding Enter (or Esc) long-presses it; the mouse wheel (turn +
// middle-click) works too. Everything lands here through one focused,
// invisible object.
static void knob_key_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_SHORT_CLICKED) {
        ui_knob_press();
        return;
    }
    if (code == LV_EVENT_LONG_PRESSED) {
        ui_knob_long_press();
        return;
    }
    // In edit mode the wheel's turns arrive as LEFT/RIGHT; on the keyboard
    // those stay unmapped.
    bool wheel = lv_indev_get_type(lv_indev_active()) == LV_INDEV_TYPE_ENCODER;
    uint32_t key = lv_event_get_key(e);
    if (key == 'p' || key == 'P') {
        cycle_sample_playback();
    } else if (key == LV_KEY_ESC) {
        ui_knob_long_press();
    } else if (key == LV_KEY_UP || (wheel && key == LV_KEY_RIGHT)) {
        ui_knob_rotate(1);
    } else if (key == LV_KEY_DOWN || (wheel && key == LV_KEY_LEFT)) {
        ui_knob_rotate(-1);
    }
}

static void knob_emulation_create(void) {
    lv_group_t *group = lv_group_create();
    lv_obj_t *sink = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(sink);
    lv_obj_set_size(sink, 0, 0);
    lv_obj_add_event_cb(sink, knob_key_cb, LV_EVENT_KEY, NULL);
    lv_obj_add_event_cb(sink, knob_key_cb, LV_EVENT_SHORT_CLICKED, NULL);
    lv_obj_add_event_cb(sink, knob_key_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_group_add_obj(group, sink);
    // Edit mode makes encoder turns arrive as LEFT/RIGHT keys instead of
    // moving focus.
    lv_group_set_editing(group, true);

    lv_indev_set_group(lv_sdl_mousewheel_create(), group);
    lv_indev_set_group(lv_sdl_keyboard_create(), group);
}

int main(void) {
    lv_init();

    lv_display_t *display = lv_sdl_window_create(WINDOW_WIDTH, WINDOW_HEIGHT);
    lv_sdl_mouse_create();

    ui_init();
    host_zone(s_settings.tz_name, sizeof(s_settings.tz_name));
    ui_update_settings(&s_settings);
    ui_set_settings_handler(on_setting);
    lights_ui_start();
    show_sample_weather();
    knob_emulation_create();
    panel_mask_create();
    lv_timer_create(clock_tick_cb, 1000, NULL);
    ui_set_pairing_handler(on_pairing);
    lv_timer_create(pairing_tick_cb, 1000, NULL);

    while (1) {
        uint32_t idle_ms = lv_timer_handler();
        lv_delay_ms(idle_ms ? idle_ms : 1);
    }

    (void)display;
    return 0;
}
