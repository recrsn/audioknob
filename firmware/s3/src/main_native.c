// Desktop entry point for the LVGL native emulator target (env:native).
// Mirrors main.c's role on real hardware, but drives an SDL window instead
// of the QSPI LCD, so the UI in ui.c can be iterated on without flashing.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "art_color.h"
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
    .art_colors = true,
    .airplay_lossless = true,
    .timer_sound = UI_TIMER_SOUND_CHIME,
    .timer_volume = 60,
    .bluetooth = true,
    .air_show = UI_AIR_SHOW_HIGH,
    .ambience_sound = UI_AMBIENCE_RAIN,
    .ambience_volume = 40,
    .focus_sound = UI_AMBIENCE_RAIN + 1,
    .tz_auto = true,
    .location = "Lisbon",
    .device_name = "AudioKnob-K7MXQ2PA",
    .hostname = "audioknob-k7mxq2pa",
    .wifi_ssid = "Sample",
    .ip = "192.168.1.50",
    .version = "native",
    .storage_total_bytes = 6 * 1024 * 1024,
    .storage_used_bytes = 384 * 1024,
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

static void show_sample_playback(void);
static void show_sample_release(void);

static void on_setting(ui_setting_t setting, int value, const char *zone) {
    switch (setting) {
    case UI_SETTING_CHECK_UPDATES:
        show_sample_release();
        break;
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
    case UI_SETTING_AIRPLAY_LOSSLESS:
        s_settings.airplay_lossless = value;
        break;
    case UI_SETTING_ART_COLORS:
        s_settings.art_colors = value;
        ui_update_settings(&s_settings);
        show_sample_playback();
        return;
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
    case UI_SETTING_DETECT_LOCATION:
        printf("settings: detect location (no-op on native)\n");
        break;
    case UI_SETTING_FORGET_WIFI:
        printf("settings: forget WiFi (no-op on native)\n");
        break;
    case UI_SETTING_BLUETOOTH:
        s_settings.bluetooth = value;
        break;
    case UI_SETTING_AIR_SHOW:
        s_settings.air_show = (uint8_t)value;
        break;
    case UI_SETTING_AMBIENCE_SOUND:
        s_settings.ambience_sound = (uint8_t)value;
        break;
    case UI_SETTING_AMBIENCE_VOLUME:
        s_settings.ambience_volume = (uint8_t)value;
        break;
    case UI_SETTING_FOCUS_SOUND:
        s_settings.focus_sound = (uint8_t)value;
        break;
    case UI_SETTING_TIMER_SOUND:
        s_settings.timer_sound = (uint8_t)value;
        break;
    case UI_SETTING_TIMER_VOLUME:
        s_settings.timer_volume = (uint8_t)value;
        break;
    }
    ui_update_settings(&s_settings);
}

// Ambient sound makes no noise here: playing is only the state the device
// would push back.
static void on_ambience_play(ui_ambience_t kind) {
    printf("ambience: play %d\n", kind);
    ui_update_ambience(true, kind);
}

static void on_ambience_stop(void) {
    printf("ambience: stop\n");
    ui_update_ambience(false, UI_AMBIENCE_BROWN);
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

// No audio here; the log shows which chime the device would play.
static void on_alert(ui_alert_t alert) {
    static const char *const names[] = {"stop", "timer", "focus end", "break end"};
    printf("alert: %s (sound %u, volume %u%%)\n", names[alert], s_settings.timer_sound, s_settings.timer_volume);
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

// Stands in for ota.c so the update screens can be laid out: U sends a
// sample image, in turn for the S3, for Bluetooth, and one that fails at
// 40%. The S3 one reads as installed straight away, as after its restart.
#define SIM_FW_TICK_MS 100
static ui_firmware_t s_fw = {.version = "", .error = ""};
static void show_sample_release(void) {
    s_fw = (ui_firmware_t){.state = UI_FIRMWARE_AVAILABLE, .version = "1.4.2", .error = ""};
    ui_update_firmware(&s_fw);
}

static unsigned s_fw_run;
static bool s_fw_fails;

static void firmware_tick_cb(lv_timer_t *timer) {
    (void)timer;
    bool moving = s_fw.state == UI_FIRMWARE_RECEIVING || s_fw.state == UI_FIRMWARE_INSTALLING;
    if (moving && s_fw.progress < 100) {
        s_fw.progress += s_fw.state == UI_FIRMWARE_RECEIVING ? 4 : 2;
        if (s_fw.progress > 100) {
            s_fw.progress = 100;
        }
        if (s_fw.state == UI_FIRMWARE_RECEIVING && s_fw_fails && s_fw.progress >= 40) {
            s_fw.state = UI_FIRMWARE_FAILED;
            s_fw.error = "Upload interrupted";
        }
    } else if (moving) {
        s_fw.state = s_fw.state == UI_FIRMWARE_RECEIVING ? UI_FIRMWARE_READY : UI_FIRMWARE_INSTALLED;
    }
    ui_update_firmware(&s_fw);
}

static void start_sample_update(void) {
    // One at a time, as on the device.
    if (s_fw.state == UI_FIRMWARE_RECEIVING || s_fw.state == UI_FIRMWARE_READY ||
        s_fw.state == UI_FIRMWARE_INSTALLING) {
        return;
    }
    s_fw_fails = s_fw_run % 3 == 2;
    s_fw = (ui_firmware_t){
        .state = UI_FIRMWARE_RECEIVING,
        .bluetooth = s_fw_run % 3 == 1,
        .version = s_fw_run % 3 == 1 ? "bt-1.4.2" : "1.4.2",
        .error = "",
    };
    s_fw_run++;
}

static void on_firmware(bool install) {
    if (install && s_fw.state == UI_FIRMWARE_AVAILABLE) {
        s_fw.state = UI_FIRMWARE_RECEIVING;
        s_fw.progress = 0;
    } else if (install && s_fw.bluetooth) {
        s_fw.state = UI_FIRMWARE_INSTALLING;
        s_fw.progress = 0;
    } else {
        s_fw.state = install ? UI_FIRMWARE_INSTALLED : UI_FIRMWARE_IDLE;
    }
}

// Fixed sample so the Weather page can be laid out without a network; the
// place name says so. A steps its air quality and UV through both over the
// threshold, one each, both under it, and none known.
static void show_sample_weather(void) {
    static const struct {
        bool has_aqi;
        uint16_t aqi;
        bool has_uv;
        float uv;
    } k_air[] = {
        {true, 162, true, 8.3f}, {true, 112, true, 2.1f}, {true, 42, true, 6.4f}, {true, 38, true, 3.0f}, {false, 0, false, 0},
    };
    static size_t step;
    ui_weather_t w = {
        .valid = true,
        .place = "Sample data",
        .temp_c = 27.4f,
        .high_c = 31.0f,
        .low_c = 21.2f,
        .code = 2,
        .is_day = true,
        .hours = {{15, 28.1f, 2, true}, {16, 27.0f, 80, true}, {17, 25.6f, 95, true}, {18, 24.2f, 3, false}},
        .has_aqi = k_air[step].has_aqi,
        .aqi = k_air[step].aqi,
        .pm2_5 = 61.0f,
        .has_uv = k_air[step].has_uv,
        .uv = k_air[step].uv,
        .uv_max = 9.1f,
    };
    ui_update_weather(&w);
    step = (step + 1) % (sizeof(k_air) / sizeof(k_air[0]));
}

// Procedural stand-ins for cover art, so the art colour can be checked: a
// warm sunset, a blue sea, a forest with a small red accent, and a grey one
// that has no colour to give (the source's tint stays).
enum { ART_SUNSET, ART_SEA, ART_FOREST, ART_GREY, ART_COUNT };

static uint16_t rgb565(float r, float g, float b) {
    return (uint16_t)((int)(r * 31.0f) << 11 | (int)(g * 63.0f) << 5 | (int)(b * 31.0f));
}

static uint16_t sample_art_pixel(int art, float x, float y) {
    float dx = x - 0.5f, dy = y - 0.62f;
    bool disc = dx * dx + dy * dy < 0.04f;
    switch (art) {
    case ART_SUNSET:
        return disc ? rgb565(1.0f, 0.85f, 0.4f) : rgb565(0.95f, 0.35f + 0.2f * y, 0.25f + 0.3f * (1.0f - y));
    case ART_SEA:
        return y > 0.55f ? rgb565(0.05f, 0.25f + 0.2f * y, 0.55f + 0.3f * y) : rgb565(0.75f, 0.85f, 0.95f);
    case ART_FOREST:
        return disc ? rgb565(0.85f, 0.15f, 0.15f) : rgb565(0.08f, 0.3f + 0.25f * ((int)(x * 9) & 1), 0.12f);
    default:
        return rgb565(0.2f + 0.6f * y, 0.2f + 0.6f * y, 0.2f + 0.6f * y);
    }
}

static const uint16_t *sample_art(int art, uint32_t *tint) {
    static uint16_t pixels[ART_COUNT][UI_ART_SIZE * UI_ART_SIZE];
    static uint32_t tints[ART_COUNT];
    static bool made[ART_COUNT];
    if (!made[art]) {
        for (int y = 0; y < UI_ART_SIZE; y++) {
            for (int x = 0; x < UI_ART_SIZE; x++) {
                pixels[art][y * UI_ART_SIZE + x] =
                    sample_art_pixel(art, (x + 0.5f) / UI_ART_SIZE, (y + 0.5f) / UI_ART_SIZE);
            }
        }
        tints[art] = art_color_dominant(pixels[art], UI_ART_SIZE, UI_ART_SIZE);
        made[art] = true;
        printf("sample art %d: colour %06x\n", art, (unsigned)tints[art]);
    }
    *tint = tints[art];
    return pixels[art];
}

// Stands in for the radio so its app and now-playing screen can be laid
// out: a tapped station connects, buffers, then "plays" a sample title.
static ui_radio_t s_radio = {
    .count = 6,
    .names = {"SomaFM Groove Salad", "Radio Paradise", "SomaFM Drone Zone", "KEXP 90.3 FM", "Sample Jazz",
              "A station with a rather long name"},
    .current = -1,
};
static uint8_t s_radio_volume = 40;
static int s_radio_ticks;

static void push_radio(void) {
    ui_update_radio(&s_radio);
    if (s_radio.current < 0) {
        return;
    }
    bool playing = s_radio.state != UI_RADIO_STOPPED;
    wireless_state_t state = {.volume = s_radio_volume, .connected = 1, .playing = playing, .source = SOURCE_RADIO};
    ui_track_info_t track = {0};
    const char *name = s_radio.names[s_radio.current];
    bool live = s_radio.state == UI_RADIO_PLAYING;
    snprintf(track.title, sizeof(track.title), "%s", live ? s_radio.detail : name);
    snprintf(track.artist, sizeof(track.artist), "%s", live ? name : s_radio.state == UI_RADIO_STOPPED ? "" : "Buffering\xE2\x80\xA6");
    snprintf(track.peer, sizeof(track.peer), "%s", name);
    ui_update_wireless(&state, &track);
}

static ui_voice_t s_voice = {.storage_ready = true};
static void on_voice_record(uint32_t peer, bool held) {
    (void)peer;
    s_voice.state = held ? UI_VOICE_RECORDING : UI_VOICE_IDLE;
    snprintf(s_voice.message, sizeof(s_voice.message), "%s", held ? "Recording" : "Voice note saved");
    ui_update_voice(&s_voice);
}
static void on_voice_play(const char *id) {
    snprintf(s_voice.id, sizeof(s_voice.id), "%s", id);
    s_voice.state = UI_VOICE_PLAYING;
    ui_update_voice(&s_voice);
}
static void on_voice_stop(void) {
    s_voice.state = UI_VOICE_IDLE;
    ui_update_voice(&s_voice);
}
static ui_talk_t s_talk = {.count = 2, .volume = 70, .peers = {{1, "AudioKnob-Priya-Desk"}, {2, "Kitchen"}}};
static ui_inbox_t s_inbox = {
    .count = 3,
    .messages = {
        {.id = "0123456789abcdef", .sender = "Priya", .text = "Tea is ready. Come downstairs when you have a moment."},
        {.id = "123456789abcdef0", .sender = "Kitchen", .text = "The doorbell rang.", .acknowledged = true},
        {.id = "223456789abcdef0", .sender = "Priya", .text = "Voice note (6 s)", .voice = true, .duration_ms = 6000}}};
static void on_talk(uint32_t peer, bool held) {
    s_talk.state = held ? UI_TALK_SENDING : UI_TALK_IDLE;
    s_talk.level = held ? 65 : 0;
    snprintf(s_talk.peer, sizeof(s_talk.peer), "%s", peer == 1 ? "AudioKnob-Priya-Desk" : "Kitchen");
    ui_update_talk(&s_talk);
}
static void on_talk_volume(uint8_t v) {
    s_talk.volume = v;
    ui_update_talk(&s_talk);
}
static void on_message_ack(const char *id) {
    for (int i = 0; i < s_inbox.count; i++)
        if (!strcmp(id, s_inbox.messages[i].id))
            s_inbox.messages[i].acknowledged = true;
    ui_update_inbox(&s_inbox);
}
static void on_message_delete(const char *id) {
    for (int i = 0; i < s_inbox.count; i++)
        if (!strcmp(id, s_inbox.messages[i].id)) {
            memmove(&s_inbox.messages[i], &s_inbox.messages[i + 1], (s_inbox.count - i - 1) * sizeof(ui_message_t));
            s_inbox.count--;
            break;
        }
    ui_update_inbox(&s_inbox);
}
static void on_radio(int station) {
    if (station < 0) {
        s_radio.state = UI_RADIO_STOPPED;
    } else {
        s_radio.current = station;
        s_radio.state = UI_RADIO_CONNECTING;
        s_radio.detail[0] = '\0';
        s_radio_ticks = 0;
    }
    push_radio();
}

static void radio_tick_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_radio.current < 0 || s_radio.state == UI_RADIO_STOPPED || s_radio.state == UI_RADIO_PLAYING) {
        return;
    }
    s_radio_ticks++;
    s_radio.state = s_radio_ticks < 2 ? UI_RADIO_CONNECTING : s_radio_ticks < 4 ? UI_RADIO_BUFFERING : UI_RADIO_PLAYING;
    if (s_radio.state == UI_RADIO_PLAYING) {
        snprintf(s_radio.detail, sizeof(s_radio.detail), "Sample Artist - Sample Song");
    }
    push_radio();
}

static void on_transport(ui_transport_t button) {
    if (s_radio.current < 0) {
        return;
    }
    if (button == UI_TRANSPORT_PLAY_PAUSE) {
        on_radio(s_radio.state == UI_RADIO_STOPPED ? s_radio.current : -1);
    } else {
        int step = button == UI_TRANSPORT_NEXT ? 1 : -1;
        on_radio(((s_radio.current + step) % s_radio.count + s_radio.count) % s_radio.count);
    }
}

// Stands in for a source so the now-playing screens can be laid out: P steps
// through nothing, Bluetooth waiting to pair, and AirPlay playing a track
// without art, then with each sample art (paused once), then Bluetooth with
// art.
static const struct {
    wireless_state_t state;
    bool track;
    int art; // -1: none
} k_steps[] = {
    {{.volume = 0, .connected = 0, .playing = 0, .source = SOURCE_AIRPLAY}, false, -1},
    {{.volume = 0, .connected = 0, .playing = 1, .source = SOURCE_A2DP}, false, -1},
    {{.volume = 45, .connected = 1, .playing = 1, .source = SOURCE_AIRPLAY}, true, -1},
    {{.volume = 45, .connected = 1, .playing = 1, .source = SOURCE_AIRPLAY}, true, ART_SUNSET},
    {{.volume = 45, .connected = 1, .playing = 0, .source = SOURCE_AIRPLAY}, true, ART_SUNSET},
    {{.volume = 45, .connected = 1, .playing = 1, .source = SOURCE_AIRPLAY}, true, ART_SEA},
    {{.volume = 45, .connected = 1, .playing = 1, .source = SOURCE_AIRPLAY}, true, ART_FOREST},
    {{.volume = 45, .connected = 1, .playing = 1, .source = SOURCE_AIRPLAY}, true, ART_GREY},
    {{.volume = 60, .connected = 1, .playing = 1, .source = SOURCE_A2DP}, true, ART_SEA},
};
#define SAMPLE_STEPS (sizeof(k_steps) / sizeof(k_steps[0]))
static size_t s_sample_step;

// Also re-run on a settings change, which the hardware's 33 ms poll covers.
static void show_sample_playback(void) {
    ui_track_info_t track = {0};
    int art = k_steps[s_sample_step].art;
    if (k_steps[s_sample_step].track) {
        track = (ui_track_info_t){
            .title = "Sample Track Title",
            .artist = "Sample Artist",
            .album = "Sample Album",
            .peer = "Sample iPhone",
        };
    }
    if (art >= 0) {
        track.art = sample_art(art, &track.art_tint);
        track.art_version = (uint32_t)art + 1;
    }
    ui_update_wireless(&k_steps[s_sample_step].state, &track);
}

static void cycle_sample_playback(void) {
    s_radio.current = -1;
    s_radio.state = UI_RADIO_STOPPED;
    ui_update_radio(&s_radio);
    s_sample_step = (s_sample_step + 1) % SAMPLE_STEPS;
    show_sample_playback();
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
    } else if (key == 't' || key == 'T') {
        s_talk.state = s_talk.state == UI_TALK_RECEIVING ? UI_TALK_IDLE : UI_TALK_RECEIVING;
        snprintf(s_talk.peer, sizeof(s_talk.peer), "AudioKnob-Priya-Desk"); s_talk.peer_id = 1; s_talk.level = 55; ui_update_talk(&s_talk);
    } else if (key == 'u' || key == 'U') {
        start_sample_update();
    } else if (key == 'a' || key == 'A') {
        show_sample_weather();
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
    ui_set_ambience_handler(&(ui_ambience_handler_t){.play = on_ambience_play, .stop = on_ambience_stop});
    ui_set_alert_handler(on_alert);
#ifdef AK_LIGHTS
    lights_ui_start();
#endif
    show_sample_weather();
    knob_emulation_create();
    panel_mask_create();
    lv_timer_create(clock_tick_cb, 1000, NULL);
    ui_set_pairing_handler(on_pairing);
    lv_timer_create(pairing_tick_cb, 1000, NULL);
    ui_set_firmware_handler(on_firmware);
    lv_timer_create(firmware_tick_cb, SIM_FW_TICK_MS, NULL);
    ui_set_radio_handler(on_radio);
    ui_set_talk_handler(on_talk, on_talk_volume); ui_update_talk(&s_talk);
    ui_set_inbox_handler(on_message_ack, on_message_delete); ui_update_inbox(&s_inbox);
    ui_set_voice_handler(on_voice_record, on_voice_play, on_voice_stop); ui_update_voice(&s_voice);
    ui_set_transport_handler(on_transport);
    ui_update_radio(&s_radio);
    lv_timer_create(radio_tick_cb, 500, NULL);

    while (1) {
        uint32_t idle_ms = lv_timer_handler();
        lv_delay_ms(idle_ms ? idle_ms : 1);
    }

    (void)display;
    return 0;
}
