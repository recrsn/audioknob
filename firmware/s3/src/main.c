#include <string.h>
#include <time.h>

#include "airplay.h"
#include "art_color.h"
#include "alarm_sound.h"
#include "audio_out.h"
#include "audio_source.h"
#include "device_name.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "focus_sound.h"
#include "haptic.h"
#include "knob.h"
#include "mic.h"
#include "talk.h"
#include "flash_message.h"
#include "message_store.h"
#include "voice_notes.h"
#include "lcd_driver.h"
#include "lights.h"
#include "lvgl.h"
#include "now_playing.h"
#include "github_release.h"
#include "ntp.h"
#include "online.h"
#include "ota.h"
#include "radio.h"
#include "settings_ui.h"
#include "uart_link.h"
#include "web_settings.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "s3";

// Once per art version (well under a millisecond), not every poll.
static uint32_t art_tint(const uint16_t *art, uint32_t version) {
    static uint32_t s_version, s_tint;
    if (version != s_version) {
        s_version = version;
        if (art) {
            int64_t start = esp_timer_get_time();
            s_tint = art_color_dominant(art, UI_ART_SIZE, UI_ART_SIZE);
            ESP_LOGI(TAG, "art colour %06lx (%lld us)", (unsigned long)s_tint, esp_timer_get_time() - start);
        } else {
            s_tint = 0;
        }
    }
    return s_tint;
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
    track_info_t wire_track;
    bool has_state = now_playing_get(&state, &wire_track);

    pairing_status_t pairing;
    ui_update_pairing(uart_link_get_pairing(&pairing) ? &pairing : NULL);

    ota_status_t ota;
    ota_get_status(&ota);
    static const ui_firmware_state_t k_firmware[] = {
        [OTA_IDLE] = UI_FIRMWARE_IDLE,           [OTA_RECEIVING] = UI_FIRMWARE_RECEIVING,
        [OTA_READY] = UI_FIRMWARE_READY,         [OTA_DECLINED] = UI_FIRMWARE_IDLE,
        [OTA_INSTALLING] = UI_FIRMWARE_INSTALLING, [OTA_INSTALLED] = UI_FIRMWARE_INSTALLED,
        [OTA_FAILED] = UI_FIRMWARE_FAILED,
    };
    github_release_status_t release;
    github_release_get_status(&release);
    bool active = ota.state == OTA_RECEIVING || ota.state == OTA_READY || ota.state == OTA_INSTALLING;
    if (active && release.notice != RELEASE_QUIET) {
        github_release_answer(false);
    }
    if (!active && release.notice != RELEASE_QUIET) {
        static const ui_firmware_state_t k_release[] = {
            [RELEASE_CHECKING] = UI_FIRMWARE_CHECKING,
            [RELEASE_AVAILABLE] = UI_FIRMWARE_AVAILABLE,
            [RELEASE_CURRENT] = UI_FIRMWARE_CURRENT,
            [RELEASE_ERROR] = UI_FIRMWARE_CHECK_FAILED,
        };
        ui_update_firmware(&(ui_firmware_t){.state = k_release[release.notice],
                                            .bluetooth = release.target == OTA_TARGET_ESP32,
                                            .version = release.target == OTA_TARGET_ESP32 ? release.bluetooth_version : release.s3_version,
                                            .error = release.error});
    } else {
        ui_update_firmware(&(ui_firmware_t){.state = k_firmware[ota.state],
                                            .bluetooth = ota.target == OTA_TARGET_ESP32,
                                            .progress = ota.progress,
                                            .version = ota.version,
                                            .error = ota.error});
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
        track.art = now_playing_artwork(&track.art_version);
        track.art_tint = art_tint(track.art, track.art ? track.art_version : 0);
        now_playing_peer_name(track.peer, sizeof(track.peer));
        ui_update_wireless(&state, &track);
    }

    ambience_kind_t kind;
    bool ambience = focus_sound_playing(&kind);
    ui_update_ambience(ambience, (ui_ambience_t)kind);
    // ~1.2 KB, so in PSRAM rather than on the LVGL task's stack.
    static ui_radio_t *radio;
    static uint32_t radio_version;
    static bool radio_pushed;
    if (!radio) {
        radio = heap_caps_malloc(sizeof(*radio), MALLOC_CAP_SPIRAM);
    }
    if (radio) {
        uint32_t rv = radio_ui_snapshot(radio);
        if (!radio_pushed || rv != radio_version) {
            radio_pushed = true;
            radio_version = rv;
            ui_update_radio(radio);
        }
    }

    static ui_inbox_t *inbox;
    if (!inbox) inbox = heap_caps_calloc(1, sizeof(*inbox), MALLOC_CAP_SPIRAM);
    if (inbox) { flash_message_snapshot(inbox); ui_update_inbox(inbox); }

    ui_voice_t voice; voice_notes_get_state(&voice); ui_update_voice(&voice);
    ui_talk_t talk;
    talk_get_state(&talk);
    ui_update_talk(&talk);

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

static void feedback(haptic_effect_t effect) {
    if (settings_ui_haptics()) {
        haptic_play(effect);
    }
}

// The app pages (lights, timers, settings) claim the knob first; otherwise
// it drives the volume of whichever source is in front. Runs on the knob
// task, hence the lock.
static void on_knob_rotate(int steps) {
    lcd_driver_lvgl_lock();
    bool used = ui_knob_rotate(steps);
    lcd_driver_lvgl_unlock();
    if (used || now_playing_adjust_volume(steps)) {
        feedback(HAPTIC_TICK);
    }
}

static void on_knob_press(void) {
    lcd_driver_lvgl_lock();
    bool used = ui_knob_press();
    lcd_driver_lvgl_unlock();
    if (!used && now_playing_airplay_in_front()) {
        airplay_toggle_play_pause();
        used = true;
    }
    if (used) {
        feedback(HAPTIC_CLICK);
    }
}

// Always the UI's: it opens the launcher, or goes home from it.
static void on_knob_long_press(void) {
    lcd_driver_lvgl_lock();
    ui_knob_long_press();
    lcd_driver_lvgl_unlock();
    feedback(HAPTIC_LONG);
}

// Taps on bare screens do nothing, so they get no click either. Runs on the
// LVGL task.
static void on_touch_clicked(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_param(e);
    if (obj && lv_obj_get_parent(obj)) {
        feedback(HAPTIC_CLICK);
    }
}

static void on_pairing(ui_pairing_action_t action) {
    static const protocol_cmd_t cmds[] = {
        [UI_PAIRING_OPEN] = CMD_PAIRING_OPEN,     [UI_PAIRING_CLOSE] = CMD_PAIRING_CLOSE,
        [UI_PAIRING_ACCEPT] = CMD_PAIRING_ACCEPT, [UI_PAIRING_REJECT] = CMD_PAIRING_REJECT,
        [UI_PAIRING_FORGET] = CMD_FORGET_DEVICES,
    };
    uart_link_send_command(cmds[action]);
}

static void on_firmware(bool install) {
    ota_status_t status;
    ota_get_status(&status);
    if (status.state != OTA_READY) {
        github_release_answer(install);
        return;
    }
    if (install) {
        ota_install();
    } else {
        ota_decline();
    }
}

static void on_alert(ui_alert_t alert) {
    if (alert == UI_ALERT_NONE) {
        haptic_alarm_stop();
        alarm_sound_stop();
        return;
    }
    if (settings_ui_haptics()) {
        haptic_alarm_start();
    }
    if (settings_ui_timer_sound()) {
        alarm_sound_start(alert == UI_ALERT_FOCUS_END   ? ALARM_SOUND_FOCUS_END
                          : alert == UI_ALERT_BREAK_END ? ALARM_SOUND_BREAK_END
                                                        : ALARM_SOUND_TIMER);
    }
}

_Static_assert((int)UI_AMBIENCE_COUNT == (int)AMBIENCE_COUNT, "ui_ambience_t mirrors ambience_kind_t");

static void on_voice_record(uint32_t peer, bool held) { char id[17]; if (held) voice_notes_record(peer, id); else voice_notes_stop(); }
static void on_voice_play(const char *id) { voice_notes_play(id); }
static void on_message_ack(const char *id) { flash_message_ack(id); }
static void on_message_delete(const char *id) { flash_message_delete(id); }

static void on_ambience_play(ui_ambience_t kind) {
    focus_sound_play((ambience_kind_t)kind);
}

// Another source started playing and took the DAC. Runs on the claiming
// task or a timer task; both stops are non-blocking.
static void on_preempted(audio_source_t source) {
    if (source == AUDIO_SOURCE_AIRPLAY) {
        airplay_preempt();
    } else if (source == AUDIO_SOURCE_RADIO) {
        radio_preempt();
    } else if (source == AUDIO_SOURCE_TALK) {
        talk_preempt();
    } else if (source == AUDIO_SOURCE_NOTE) {
        voice_notes_preempt();
    }
}

static void on_network_up(const uint8_t mac[6]) {
    airplay_network_up(mac);
    online_network_up();
    talk_network_up();
    // The web page is how the next update arrives, so a new image that
    // gets this far is kept.
    if (web_settings_start()) {
        ota_confirm_running();
    }
}

// Runs at init and on the web server's task after a rename.
static void on_device_name(const char *name) {
    uart_link_set_device_name(name);
    airplay_rename();
    talk_rename();
}

void app_main(void) {
    lcd_driver_backlight_init();
    ESP_LOGI(TAG, "ESP32-S3 target boot");
    ntp_restore_rtc();

    uart_link_start();

    audio_out_init();
    mic_init();
    airplay_init();
    // Past boot strapping, so GPIO0 (CH445P select) is safe to drive now.
    audio_source_init(on_preempted);
    focus_sound_init();
    alarm_sound_init();

    lcd_driver_init();
    haptic_init(lcd_driver_i2c_bus());
    online_start();
    settings_ui_init();
    // After settings, which brings NVS up.
    radio_init();
    message_store_init();
    flash_message_init();
    voice_notes_init();
    talk_init();
    device_name_init(on_device_name);
    lcd_driver_lvgl_lock();
    ui_init();
    ui_set_transport_handler(now_playing_transport);
    ui_set_source_handler(now_playing_pick);
    ui_set_radio_handler(radio_play);
    ui_set_talk_handler(talk_press, talk_set_volume);
    ui_set_inbox_handler(on_message_ack, on_message_delete);
    ui_set_voice_handler(on_voice_record, on_voice_play, voice_notes_stop);
    ui_set_pairing_handler(on_pairing);
    ui_set_firmware_handler(on_firmware);
    ui_set_alert_handler(on_alert);
    ui_set_display_handler(lcd_driver_set_backlight_on);
    ui_set_ambience_handler(&(ui_ambience_handler_t){.play = on_ambience_play, .stop = focus_sound_stop});
    lv_indev_add_event_cb(lcd_driver_touch_indev(), on_touch_clicked, LV_EVENT_CLICKED, NULL);
    settings_ui_start();
#ifdef AK_LIGHTS
    lights_ui_start();
#endif
    lv_timer_create(state_poll_timer_cb, 33, NULL);
    lcd_driver_show_ui();
    lcd_driver_lvgl_unlock();

    knob_start(on_knob_rotate, on_knob_press, on_knob_long_press);

    wifi_start(on_network_up);
    github_release_start();
    ntp_start();
}
