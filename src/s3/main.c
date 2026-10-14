#include <string.h>
#include <time.h>

#include "airplay.h"
#include "audio_out.h"
#include "audio_source.h"
#include "device_name.h"
#include "esp_log.h"
#include "haptic.h"
#include "knob.h"
#include "lcd_driver.h"
#include "lights.h"
#include "lvgl.h"
#include "ntp.h"
#include "online.h"
#include "settings_ui.h"
#include "uart_link.h"
#include "web_settings.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "s3";

// Manual pick from the UI's switch screen. It holds only while the DAC
// stays where the pick put it: if the other source then starts playing,
// "last to start wins" takes over and the pick is dropped.
typedef enum { PICK_NONE, PICK_AIRPLAY, PICK_BLUETOOTH } source_pick_t;
static volatile source_pick_t s_pick = PICK_NONE;
static audio_source_t s_pick_dac;

// AirPlay is "in front" -- its state on screen, the S3 knob driving it --
// while it holds the DAC, or while it has a session open and A2DP is idle.
// Otherwise the ESP32 owns wireless controls and this MCU only displays.
static bool airplay_in_front(void) {
    if (s_pick != PICK_NONE) {
        if (audio_source_active() == s_pick_dac) {
            return s_pick == PICK_AIRPLAY;
        }
        s_pick = PICK_NONE;
    }
    if (audio_source_active() == AUDIO_SOURCE_AIRPLAY) {
        return true;
    }
    wireless_state_t esp32;
    return airplay_is_connected() && !(uart_link_get_latest(&esp32) && esp32.playing);
}

// Polls, rather than being pushed to, per spec.md's State Sync Protocol:
// producers (UART RX task, AirPlay) only write their own shared state and
// never touch ui.c. Runs on the LVGL task's own timer (lcd_driver_init()
// already holds the LVGL lock for the whole lv_timer_handler() call this
// fires inside of), at the same ~33ms cadence the ESP32 side rate-limits
// its outgoing STATE_UPDATE frames to.
static void state_poll_timer_cb(lv_timer_t *timer) {
    (void)timer;

    wireless_state_t state;
    track_info_t wire_track = {0};
    bool has_state = true;

    if (airplay_in_front()) {
        airplay_get_state(&state, &wire_track);
    } else if ((has_state = uart_link_get_latest(&state))) {
        // Fine if this comes back false (nothing has played since boot) --
        // wire_track just stays zeroed, which ui_update_wireless() renders
        // as "Not playing".
        uart_link_get_latest_track(&wire_track);
    }

    wifi_setup_info_t setup;
    if (wifi_get_setup_info(&setup)) {
        ui_show_wifi_setup(setup.qr_payload, setup.ap_ssid, setup.ap_password, setup.status);
    }

    if (has_state) {
        ui_track_info_t track = {0};
        memcpy(track.title, wire_track.title, sizeof(track.title));
        memcpy(track.artist, wire_track.artist, sizeof(track.artist));
        memcpy(track.album, wire_track.album, sizeof(track.album));
        track.art = airplay_in_front() ? airplay_get_artwork(&track.art_version)
                                       : uart_link_get_artwork(&track.art_version);
        ui_update_wireless(&state, &track);
    }

    static uint32_t weather_version;
    ui_weather_t weather;
    uint32_t v = online_get_weather(&weather);
    if (v != weather_version) {
        weather_version = v;
        ui_update_weather(&weather);
    }

    time_t now = time(NULL);
    struct tm local_time;
    localtime_r(&now, &local_time);
    ui_update_clock(ntp_is_synced(), &local_time);
}

// The app pages (lights, timers, settings) claim the knob first; otherwise
// it drives the volume of whichever source is in front. Runs on the knob
// task, hence the lock.
static void on_knob_rotate(int steps) {
    lcd_driver_lvgl_lock();
    bool used = ui_knob_rotate(steps);
    lcd_driver_lvgl_unlock();
    if (used) {
        return;
    }
    if (airplay_in_front()) {
        airplay_adjust_volume(steps);
        return;
    }
    wireless_state_t esp32;
    if (uart_link_get_latest(&esp32) && esp32.connected) {
        // A detent at a time, so Bluetooth moves 1% per step just like AirPlay.
        protocol_cmd_t cmd = steps > 0 ? CMD_VOLUME_UP : CMD_VOLUME_DOWN;
        for (int i = steps > 0 ? steps : -steps; i > 0; i--) {
            uart_link_send_command(cmd);
        }
    }
}

static void on_knob_press(void) {
    lcd_driver_lvgl_lock();
    bool used = ui_knob_press();
    lcd_driver_lvgl_unlock();
    if (!used && airplay_in_front()) {
        airplay_toggle_play_pause();
    }
}

// Always the UI's: it opens the launcher, or goes home from it.
static void on_knob_long_press(void) {
    lcd_driver_lvgl_lock();
    ui_knob_long_press();
    lcd_driver_lvgl_unlock();
}

static void on_transport(ui_transport_t button) {
    if (airplay_in_front()) {
        switch (button) {
        case UI_TRANSPORT_PREV:
            airplay_prev();
            break;
        case UI_TRANSPORT_PLAY_PAUSE:
            airplay_toggle_play_pause();
            break;
        case UI_TRANSPORT_NEXT:
            airplay_next();
            break;
        }
        return;
    }
    static const protocol_cmd_t cmds[] = {[UI_TRANSPORT_PREV] = CMD_PREV,
                                          [UI_TRANSPORT_PLAY_PAUSE] = CMD_PLAY_PAUSE,
                                          [UI_TRANSPORT_NEXT] = CMD_NEXT};
    uart_link_send_command(cmds[button]);
}

static void on_alert(bool ringing) {
    if (ringing && settings_ui_haptics()) {
        haptic_alarm_start();
    } else {
        haptic_alarm_stop();
    }
}

static void on_network_up(const uint8_t mac[6]) {
    airplay_network_up(mac);
    online_network_up();
    web_settings_start();
}

// Runs at init and on the web server's task after a rename.
static void on_device_name(const char *name) {
    uart_link_set_device_name(name);
    airplay_rename();
}

static void on_source_pick(ui_source_t source) {
    if (source == UI_SOURCE_AIRPLAY) {
        audio_source_claim_airplay();
        s_pick = PICK_AIRPLAY;
    } else {
        airplay_preempt();
        audio_source_release_airplay();
        s_pick = PICK_BLUETOOTH;
    }
    s_pick_dac = audio_source_active();
}

void app_main(void) {
    ESP_LOGI(TAG, "ESP32-S3 target boot");
    ntp_restore_rtc();

    uart_link_start();

    audio_out_init();
    airplay_init();
    // Past boot strapping, so GPIO0 (CH445P select) is safe to drive now.
    audio_source_init(airplay_preempt);

    lcd_driver_init();
    haptic_init(lcd_driver_i2c_bus());
    online_start();
    settings_ui_init();
    device_name_init(on_device_name);
    lcd_driver_lvgl_lock();
    ui_init();
    ui_set_transport_handler(on_transport);
    ui_set_source_handler(on_source_pick);
    ui_set_alert_handler(on_alert);
    settings_ui_start();
    lights_ui_start();
    lv_timer_create(state_poll_timer_cb, 33, NULL);
    lcd_driver_lvgl_unlock();

    knob_start(on_knob_rotate, on_knob_press, on_knob_long_press);

    wifi_start(on_network_up);
    ntp_start();
}
