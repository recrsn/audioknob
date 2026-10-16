#include "ui_tools.h"

#include <stdio.h>
#include <string.h>

#include "tz.h"

// A list under the title: the knob moves the selection (the list scrolls to
// keep it near the middle) and a press, or a tap on a row, acts on it.
// Switches flip at once; brightness, sleep and timezone are edited in place:
// the knob changes the value and a cancel/apply strip covers the rows below
// until one is tapped (a press applies too). The name and web page rows (and turning Auto location off) open a
// QR code for the device's web page, which has every setting too (a name or
// a city can't be typed here). The Forget rows ask first, on the
// full-screen confirmation.

#define SET_TITLE_Y     UI_TITLE_TOP_Y
#define SET_LIST_W      272
// From just under the title to the home pill.
#define SET_LIST_TOP    (UI_SAFE_TOP + 22)
#define SET_LIST_H      (304 - SET_LIST_TOP)
#define SET_ROW_H       48
#define SET_HEADER_H    28
#define SET_ROW_PAD     UI_SPACE_L
#define SET_SWITCH_W    44
#define SET_SWITCH_H    26
#define SET_KNOB        22
#define SET_LEVEL_W     52
#define SET_FADE_ROWS   3   // rows from the selection to the faintest
#define SET_FADE_MIN    40
#define SET_BRIGHT_STEP 5
#define SET_BRIGHT_MIN  5
// The edit mode's cancel/apply strip, from under the edited row to the
// list's end.
#define SET_EDIT_GAP_TOP UI_SPACE_XS
#define SET_EDIT_BTN    UI_BUTTON_SIZE
#define SET_EDIT_GAP    32
// Timezone picker position for "automatic", ahead of tz_db's entries.
#define TZ_AUTO (-1)
// Display sleep choices, seconds; the web page offers the same.
static const uint16_t k_sleep_options[] = {0, 30, 60, 120, 300, 600, 1800, UI_SLEEP_MAX_S};
#define SLEEP_OPTION_COUNT (int)(sizeof(k_sleep_options) / sizeof(k_sleep_options[0]))
#define SET_QR_SIZE     150
#define SET_QR_Y        (-4)
#define SET_QR_TITLE_Y  UI_SAFE_TOP_Y(20) // inter_semi_16, clear of the audio chip
#define SET_QR_HINT_Y   100
#define SET_QR_URL_Y    122

typedef enum {
    ROW_BRIGHTNESS,
    ROW_SLEEP,
    ROW_HAPTICS,
    ROW_CLOCK,
    ROW_LOCATION,
    ROW_TIMEZONE,
    ROW_UNITS,
    ROW_AIRPLAY2,
    ROW_AIRPLAY1,
    ROW_BLUETOOTH,
    ROW_BT_PAIR,
    ROW_BT_FORGET,
    ROW_NAME,
    ROW_WIFI,
    ROW_WEB,
    ROW_FORGET,
    ROW_VERSION,
    ROW_MEMORY,
    ROW_COUNT,
} row_t;

typedef enum {
    KIND_VALUE,  // a label on the right
    KIND_SWITCH,
    KIND_LEVEL,  // a bar and a percentage
} row_kind_t;

static const struct {
    const char *name;
    row_kind_t kind;
} k_rows[ROW_COUNT] = {
    [ROW_BRIGHTNESS] = {"Brightness", KIND_LEVEL},
    [ROW_SLEEP] = {"Sleep after", KIND_VALUE},
    [ROW_HAPTICS] = {"Haptics", KIND_SWITCH},
    [ROW_CLOCK] = {"Clock", KIND_VALUE},
    [ROW_LOCATION] = {"Auto location", KIND_SWITCH},
    [ROW_TIMEZONE] = {"Timezone", KIND_VALUE},
    [ROW_UNITS] = {"Units", KIND_VALUE},
    [ROW_AIRPLAY2] = {"AirPlay 2", KIND_SWITCH},
    [ROW_AIRPLAY1] = {"AirPlay 1", KIND_SWITCH},
    [ROW_BLUETOOTH] = {"Bluetooth", KIND_SWITCH},
    [ROW_BT_PAIR] = {"Pair a device", KIND_VALUE},
    [ROW_BT_FORGET] = {"Forget devices", KIND_VALUE},
    [ROW_NAME] = {"Name", KIND_VALUE},
    [ROW_WIFI] = {"WiFi", KIND_VALUE},
    [ROW_WEB] = {"Web page", KIND_VALUE},
    [ROW_FORGET] = {"Forget WiFi", KIND_VALUE},
    [ROW_VERSION] = {"Version", KIND_VALUE},
    [ROW_MEMORY] = {"Free memory", KIND_VALUE},
};

// Top to bottom: a header (row < 0) or a row.
#define HEADER(text) {text, -1}
static const struct {
    const char *header;
    int row;
} k_layout[] = {
    HEADER("DISPLAY"),
    {NULL, ROW_BRIGHTNESS},
    {NULL, ROW_SLEEP},
    {NULL, ROW_HAPTICS},
    HEADER("TIME AND WEATHER"),
    {NULL, ROW_CLOCK},
    {NULL, ROW_LOCATION},
    {NULL, ROW_TIMEZONE},
    {NULL, ROW_UNITS},
    HEADER("AUDIO"),
    {NULL, ROW_AIRPLAY2},
    {NULL, ROW_AIRPLAY1},
    {NULL, ROW_BLUETOOTH},
    {NULL, ROW_BT_PAIR},
    {NULL, ROW_BT_FORGET},
    HEADER("NETWORK"),
    {NULL, ROW_NAME},
    {NULL, ROW_WIFI},
    {NULL, ROW_WEB},
    {NULL, ROW_FORGET},
    HEADER("ABOUT"),
    {NULL, ROW_VERSION},
    {NULL, ROW_MEMORY},
};
#define LAYOUT_COUNT (sizeof(k_layout) / sizeof(k_layout[0]))

static ui_settings_t s_settings = {
    .brightness = 80,
    .sleep_s = 300,
    .clock_24h = true,
    .haptics = true,
    .bluetooth = true,
    .airplay = UI_AIRPLAY_1 | UI_AIRPLAY_2,
    .tz_auto = true,
    .location_auto = true,
};

static struct {
    lv_obj_t *list;
    lv_obj_t *items[LAYOUT_COUNT];
    lv_obj_t *rows[ROW_COUNT];
    lv_obj_t *names[ROW_COUNT];
    lv_obj_t *values[ROW_COUNT];   // KIND_VALUE and KIND_LEVEL
    lv_obj_t *switches[ROW_COUNT]; // KIND_SWITCH: the track; its child is the knob
    lv_obj_t *level;               // brightness bar
    lv_obj_t *edit_bar;            // cancel/apply, while editing
    int selected;
    bool editing;
    uint16_t edit_from;            // brightness or sleep_s when editing began
    int tz_pick;                   // TZ_AUTO or a tz_db index, while editing the timezone
    lv_obj_t *qr_overlay; // while open
    lv_obj_t *qr;
    lv_obj_t *qr_url;
    void (*handler)(ui_setting_t setting, int value, const char *zone);
} st;

const ui_settings_t *ui_settings_current(void) {
    return &s_settings;
}

void ui_set_settings_handler(void (*handler)(ui_setting_t setting, int value, const char *zone)) {
    st.handler = handler;
}

static void emit(ui_setting_t setting, int value, const char *zone) {
    if (st.handler) {
        st.handler(setting, value, zone);
    }
}

static bool row_visible(int row) {
    // Without the AirPlay 2 receiver, the AirPlay 1 switch is all there is.
    return row != ROW_AIRPLAY2 || s_settings.airplay2_available;
}

// "America/Argentina/Buenos_Aires" -> "Buenos Aires": the city is what
// people recognise, and the full name rarely fits.
static void zone_city(const char *zone, char *out, size_t len) {
    const char *slash = strrchr(zone, '/');
    snprintf(out, len, "%s", slash ? slash + 1 : zone);
    for (char *c = out; *c; c++) {
        if (*c == '_') {
            *c = ' ';
        }
    }
}

static void timezone_text(char *out, size_t len) {
    char city[40];
    if (st.editing && st.selected == ROW_TIMEZONE) {
        if (st.tz_pick == TZ_AUTO) {
            snprintf(out, len, LV_SYMBOL_LEFT " Automatic " LV_SYMBOL_RIGHT);
        } else {
            zone_city(tz_db[st.tz_pick].name, city, sizeof(city));
            snprintf(out, len, LV_SYMBOL_LEFT " %s " LV_SYMBOL_RIGHT, city);
        }
        return;
    }
    zone_city(s_settings.tz_name, city, sizeof(city));
    if (s_settings.tz_auto) {
        snprintf(out, len, s_settings.tz_name[0] ? "Auto \xC2\xB7 %s" : "Auto", city);
    } else {
        snprintf(out, len, "%s", city);
    }
}

// NULL while Bluetooth is off or the ESP32 isn't answering.
static const pairing_status_t *bluetooth_status(void) {
    return s_settings.bluetooth ? ui_pairing_status() : NULL;
}

static void bt_pair_text(char *buf, size_t len) {
    const pairing_status_t *bt = bluetooth_status();
    if (!s_settings.bluetooth) {
        snprintf(buf, len, "Bluetooth off");
    } else if (!bt) {
        snprintf(buf, len, "Starting\xE2\x80\xA6");
    } else if (bt->bonded) {
        snprintf(buf, len, "%u paired", bt->bonded);
    } else {
        snprintf(buf, len, "None yet");
    }
}

static bool switch_on(int row) {
    const ui_settings_t *s = &s_settings;
    switch (row) {
    case ROW_HAPTICS:
        return s->haptics;
    case ROW_AIRPLAY2:
        return s->airplay & UI_AIRPLAY_2;
    case ROW_AIRPLAY1:
        return s->airplay & UI_AIRPLAY_1;
    case ROW_BLUETOOTH:
        return s->bluetooth;
    case ROW_LOCATION:
        return s->location_auto;
    default:
        return false;
    }
}

static void switch_set(lv_obj_t *track, bool on) {
    lv_obj_set_style_bg_color(track, lv_color_hex(on ? UI_COLOR_ACCENT : UI_COLOR_CONTROL), 0);
    lv_obj_align(lv_obj_get_child(track, 0), on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
}

static void sleep_text(uint16_t s, char *out, size_t len) {
    if (!s) {
        snprintf(out, len, "Never");
    } else if (s < 60) {
        snprintf(out, len, "%u s", s);
    } else if (s < 3600) {
        snprintf(out, len, "%u min", s / 60);
    } else {
        snprintf(out, len, "%u h", s / 3600);
    }
}

// The first choice at least as long as `s`, so a value set elsewhere steps
// from where it is.
static int sleep_option(uint16_t s) {
    for (int i = 0; i < SLEEP_OPTION_COUNT; i++) {
        if (k_sleep_options[i] >= s) {
            return i;
        }
    }
    return SLEEP_OPTION_COUNT - 1;
}

static void value_text(int row, char *buf, size_t len) {
    const ui_settings_t *s = &s_settings;
    bool editing = st.editing && st.selected == row;
    switch (row) {
    case ROW_BRIGHTNESS:
        snprintf(buf, len, editing ? LV_SYMBOL_LEFT " %u%% " LV_SYMBOL_RIGHT : "%u%%", s->brightness);
        break;
    case ROW_SLEEP: {
        char text[16];
        sleep_text(s->sleep_s, text, sizeof(text));
        snprintf(buf, len, editing ? LV_SYMBOL_LEFT " %s " LV_SYMBOL_RIGHT : "%s", text);
        break;
    }
    case ROW_CLOCK:
        snprintf(buf, len, "%s", s->clock_24h ? "24-hour" : "12-hour");
        break;
    case ROW_UNITS:
        snprintf(buf, len, "%s", s->fahrenheit ? "\xC2\xB0" "F" : "\xC2\xB0" "C");
        break;
    case ROW_TIMEZONE:
        timezone_text(buf, len);
        break;
    case ROW_NAME:
        snprintf(buf, len, "%s", s->device_name);
        break;
    case ROW_WIFI:
        snprintf(buf, len, "%s", s->wifi_ssid[0] ? s->wifi_ssid : "Not connected");
        break;
    case ROW_WEB:
        snprintf(buf, len, "%s", s->ip[0] ? s->ip : "Offline");
        break;
    case ROW_BT_PAIR:
        bt_pair_text(buf, len);
        break;
    case ROW_VERSION:
        snprintf(buf, len, "%s", s->version[0] ? s->version : "dev");
        break;
    case ROW_MEMORY:
        snprintf(buf, len, "%lu KB \xC2\xB7 %lu.%lu MB", (unsigned long)s->free_internal_kb,
                 (unsigned long)s->free_psram_kb / 1024, (unsigned long)s->free_psram_kb % 1024 * 10 / 1024);
        break;
    default:
        buf[0] = '\0';
        break;
    }
}

static void render(void) {
    char buf[64];
    for (int i = 0; i < ROW_COUNT; i++) {
        if (st.switches[i]) {
            switch_set(st.switches[i], switch_on(i));
            continue;
        }
        value_text(i, buf, sizeof(buf));
        ui_label_set_text(st.values[i], buf);
        uint32_t color = i == ROW_FORGET || i == ROW_BT_FORGET ? UI_COLOR_DANGER : st.editing && st.selected == i ? UI_COLOR_ACCENT : UI_COLOR_TEXT_DIM;
        lv_obj_set_style_text_color(st.values[i], lv_color_hex(color), 0);
    }
    lv_obj_set_style_text_color(st.names[ROW_FORGET], lv_color_hex(UI_COLOR_DANGER), 0);
    lv_obj_set_style_text_color(st.names[ROW_BT_FORGET], lv_color_hex(UI_COLOR_DANGER), 0);
    lv_obj_set_style_text_color(st.names[ROW_AIRPLAY1], lv_color_hex(UI_COLOR_TEXT), 0);
    ui_label_set_text(st.names[ROW_AIRPLAY1], s_settings.airplay2_available ? "AirPlay 1" : "AirPlay");
    lv_bar_set_value(st.level, s_settings.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(st.level, lv_color_hex(st.editing && st.selected == ROW_BRIGHTNESS ? UI_COLOR_ACCENT : UI_COLOR_TEXT),
                              LV_PART_INDICATOR);
    for (int i = 0; i < ROW_COUNT; i++) {
        bool selected = i == st.selected;
        lv_obj_set_style_bg_opa(st.rows[i], selected ? (st.editing ? UI_OPA_EDITING : UI_OPA_SELECTED) : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(st.rows[i], lv_color_hex(selected && st.editing ? UI_COLOR_ACCENT : UI_COLOR_TEXT), 0);
    }
    ui_set_visible(st.edit_bar, st.editing);
}

// Rows fade with their distance from the selection. Labels and switches
// each take their own opacity: a whole-row opa would render every row
// through a layer.
static void fade(void) {
    lv_obj_update_layout(st.list);
    lv_area_t sel;
    lv_obj_get_coords(st.rows[st.selected], &sel);
    int32_t mid = (sel.y1 + sel.y2) / 2;
    for (size_t i = 0; i < LAYOUT_COUNT; i++) {
        lv_obj_t *item = st.items[i];
        if (lv_obj_has_flag(item, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }
        lv_area_t a;
        lv_obj_get_coords(item, &a);
        int32_t dist = LV_ABS((a.y1 + a.y2) / 2 - mid);
        int32_t opa = LV_OPA_COVER - (LV_OPA_COVER - SET_FADE_MIN) * dist / (SET_ROW_H * SET_FADE_ROWS);
        opa = opa < SET_FADE_MIN ? SET_FADE_MIN : opa;
        lv_obj_set_style_text_opa(item, (lv_opa_t)opa, 0);
        int row = k_layout[i].row;
        if (row >= 0 && st.switches[row]) {
            lv_obj_set_style_opa(st.switches[row], (lv_opa_t)opa, 0);
        } else if (row == ROW_BRIGHTNESS) {
            lv_obj_set_style_opa(st.level, (lv_opa_t)opa, 0);
        }
    }
}

// Scrolls a row toward the middle, as far as the list's ends allow: the
// first rows stay where they are, under the title.
static void center_row(int row, lv_anim_enable_t anim) {
    lv_obj_update_layout(st.list);
    lv_area_t list_area, row_area;
    lv_obj_get_coords(st.list, &list_area);
    lv_obj_get_coords(st.rows[row], &row_area);
    int32_t y = lv_obj_get_scroll_y(st.list);
    int32_t target = y + (row_area.y1 + row_area.y2) / 2 - (list_area.y1 + list_area.y2) / 2;
    int32_t max = y + lv_obj_get_scroll_bottom(st.list);
    target = target < 0 ? 0 : target > max ? max : target;
    lv_obj_scroll_to_y(st.list, target, anim);
}

static void select_row(int row, bool scroll) {
    if (row < 0 || row >= ROW_COUNT || !row_visible(row)) {
        return;
    }
    if (row != st.selected) {
        st.selected = row;
        render();
        fade();
    }
    if (scroll) {
        center_row(row, LV_ANIM_ON);
    }
}

// Next visible row `steps` away in layout order, clamped to the ends.
static int row_after(int row, int steps) {
    int pos = 0;
    while (k_layout[pos].row != row) {
        pos++;
    }
    int dir = steps > 0 ? 1 : -1;
    for (int n = steps > 0 ? steps : -steps; n > 0; n--) {
        int next = pos + dir;
        while (next >= 0 && next < (int)LAYOUT_COUNT && (k_layout[next].row < 0 || !row_visible(k_layout[next].row))) {
            next += dir;
        }
        if (next < 0 || next >= (int)LAYOUT_COUNT) {
            break;
        }
        pos = next;
    }
    return k_layout[pos].row;
}

// The device page by IP: .local names don't resolve on every phone.
static void qr_show_url(void) {
    char url[32];
    snprintf(url, sizeof(url), "http://%s/", s_settings.ip);
    lv_qrcode_update(st.qr, url, strlen(url));
    lv_label_set_text_fmt(st.qr_url, "%s.local", s_settings.hostname);
}

static void qr_close(void) {
    if (st.qr_overlay) {
        lv_obj_delete(st.qr_overlay);
        st.qr_overlay = NULL;
    }
}

static void qr_click_cb(lv_event_t *e) {
    (void)e;
    qr_close();
}

static lv_obj_t *qr_label(const lv_font_t *font, lv_opa_t opa, int32_t y) {
    lv_obj_t *label = lv_label_create(st.qr_overlay);
    lv_obj_set_width(label, UI_TEXT_W);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    return label;
}

static void qr_open(lv_obj_t *screen, const char *title) {
    st.qr_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(st.qr_overlay);
    lv_obj_set_size(st.qr_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(st.qr_overlay, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(st.qr_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(st.qr_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(st.qr_overlay, qr_click_cb, LV_EVENT_CLICKED, NULL);

    lv_label_set_text(qr_label(UI_FONT_TITLE, LV_OPA_COVER, SET_QR_TITLE_Y), title);
    st.qr = lv_qrcode_create(st.qr_overlay);
    lv_qrcode_set_size(st.qr, SET_QR_SIZE);
    lv_qrcode_set_dark_color(st.qr, lv_color_black());
    lv_qrcode_set_light_color(st.qr, lv_color_white());
    lv_qrcode_set_quiet_zone(st.qr, true);
    lv_obj_align(st.qr, LV_ALIGN_CENTER, 0, SET_QR_Y);
    lv_label_set_text(qr_label(UI_FONT_BODY, LV_OPA_COVER, SET_QR_HINT_Y), "Scan on the same WiFi");
    st.qr_url = qr_label(UI_FONT_CAPTION, UI_OPA_TEXT_MUTED, SET_QR_URL_Y);
    qr_show_url();
}

static void edit_end(bool apply);

// Leaving the page (e.g. for another app) closes the code and cancels an
// edit.
static void screen_unloaded_cb(lv_event_t *e) {
    (void)e;
    qr_close();
    edit_end(false);
}

void ui_settings_refresh(void) {
    if (st.list) {
        render();
    }
}

void ui_update_settings(const ui_settings_t *settings) {
    if (!memcmp(settings, &s_settings, sizeof(s_settings))) {
        return;
    }
    bool url_changed = strcmp(settings->ip, s_settings.ip) || strcmp(settings->hostname, s_settings.hostname);
    bool layout_changed = settings->airplay2_available != s_settings.airplay2_available;
    s_settings = *settings;
    if (layout_changed) {
        lv_obj_set_flag(st.rows[ROW_AIRPLAY2], LV_OBJ_FLAG_HIDDEN, !row_visible(ROW_AIRPLAY2));
        if (!row_visible(st.selected)) {
            st.selected = ROW_AIRPLAY1;
        }
        center_row(st.selected, LV_ANIM_OFF);
    }
    render();
    if (st.qr_overlay && url_changed) {
        // Dropped off WiFi: the code would point nowhere.
        if (!s_settings.ip[0]) {
            qr_close();
        } else {
            qr_show_url();
        }
    }
}

static void toggle_airplay(unsigned bit) {
    emit(UI_SETTING_AIRPLAY, (s_settings.airplay ^ bit) & (UI_AIRPLAY_1 | UI_AIRPLAY_2), NULL);
}

// Covers the rows below the edited one, which is settled in place first so
// the strip lines up with it.
static void edit_bar_place(void) {
    center_row(st.selected, LV_ANIM_OFF);
    lv_area_t row;
    lv_obj_get_coords(st.rows[st.selected], &row);
    int32_t top = row.y2 + SET_EDIT_GAP_TOP;
    lv_obj_set_height(st.edit_bar, SET_LIST_TOP + SET_LIST_H - top);
    lv_obj_align(st.edit_bar, LV_ALIGN_TOP_MID, 0, top);
}

static void edit_begin(void) {
    edit_bar_place();
    st.editing = true;
    st.edit_from = st.selected == ROW_BRIGHTNESS ? s_settings.brightness : s_settings.sleep_s;
    if (st.selected == ROW_TIMEZONE) {
        st.tz_pick = s_settings.tz_auto ? TZ_AUTO : tz_db_find(s_settings.tz_name);
    }
}

// Brightness and sleep change as the knob turns, so cancelling puts them
// back; the timezone only takes effect once applied.
static void edit_end(bool apply) {
    if (!st.editing) {
        return;
    }
    st.editing = false;
    if (st.selected == ROW_TIMEZONE && apply) {
        emit(UI_SETTING_TIMEZONE, 0, st.tz_pick == TZ_AUTO ? NULL : tz_db[st.tz_pick].name);
    } else if (st.selected == ROW_BRIGHTNESS && !apply) {
        emit(UI_SETTING_BRIGHTNESS, st.edit_from, NULL);
    } else if (st.selected == ROW_SLEEP && !apply) {
        emit(UI_SETTING_SLEEP, st.edit_from, NULL);
    }
    render();
}

// Run once their confirmation is accepted.
static void forget_devices(void) {
    ui_pairing_request(UI_PAIRING_FORGET);
}

static void forget_wifi(void) {
    emit(UI_SETTING_FORGET_WIFI, 0, NULL);
}

static void activate(void) {
    const ui_settings_t *s = &s_settings;
    if (st.qr_overlay) {
        qr_close();
        return;
    }
    if (st.editing) {
        edit_end(true);
        return;
    }
    switch (st.selected) {
    case ROW_BRIGHTNESS:
    case ROW_SLEEP:
    case ROW_TIMEZONE:
        edit_begin();
        break;
    case ROW_CLOCK:
        emit(UI_SETTING_CLOCK_24H, !s->clock_24h, NULL);
        break;
    case ROW_UNITS:
        emit(UI_SETTING_FAHRENHEIT, !s->fahrenheit, NULL);
        break;
    case ROW_HAPTICS:
        emit(UI_SETTING_HAPTICS, !s->haptics, NULL);
        break;
    case ROW_BLUETOOTH:
        emit(UI_SETTING_BLUETOOTH, !s->bluetooth, NULL);
        break;
    case ROW_BT_PAIR:
        if (bluetooth_status()) {
            ui_pairing_request(UI_PAIRING_OPEN);
        }
        break;
    case ROW_BT_FORGET: {
        const pairing_status_t *bt = bluetooth_status();
        if (bt && bt->bonded) {
            char body[80];
            snprintf(body, sizeof(body), "%u %s will have to pair again", bt->bonded,
                     bt->bonded == 1 ? "device" : "devices");
            ui_confirm(LV_SYMBOL_BLUETOOTH, "Forget paired devices?", body, forget_devices);
        }
        break;
    }
    case ROW_AIRPLAY2:
        toggle_airplay(UI_AIRPLAY_2);
        break;
    case ROW_AIRPLAY1:
        toggle_airplay(UI_AIRPLAY_1);
        break;
    case ROW_LOCATION:
        // A city can't be typed here, so turning it off sends the user to
        // the web page to pick one; turning it on is immediate.
        if (!s->location_auto) {
            emit(UI_SETTING_LOCATION, 0, NULL);
        } else if (s->ip[0]) {
            qr_open(lv_obj_get_screen(st.list), "Set location on your phone");
        }
        break;
    case ROW_NAME:
    case ROW_WEB:
        if (s->ip[0]) {
            qr_open(lv_obj_get_screen(st.list), st.selected == ROW_NAME ? "Rename on your phone" : "Settings on your phone");
        }
        break;
    case ROW_FORGET:
        ui_confirm(LV_SYMBOL_WIFI, "Forget WiFi?", "The device restarts and opens its setup hotspot", forget_wifi);
        break;
    default:
        break;
    }
    render();
}

static bool settings_rotate(int steps) {
    if (st.qr_overlay) {
        return true;
    }
    if (!st.editing) {
        select_row(row_after(st.selected, steps), true);
        return true;
    }
    if (st.selected == ROW_BRIGHTNESS) {
        int pct = s_settings.brightness + steps * SET_BRIGHT_STEP;
        pct = pct < SET_BRIGHT_MIN ? SET_BRIGHT_MIN : pct > 100 ? 100 : pct;
        emit(UI_SETTING_BRIGHTNESS, pct, NULL);
    } else if (st.selected == ROW_SLEEP) {
        int i = sleep_option(s_settings.sleep_s) + steps;
        i = i < 0 ? 0 : i >= SLEEP_OPTION_COUNT ? SLEEP_OPTION_COUNT - 1 : i;
        emit(UI_SETTING_SLEEP, k_sleep_options[i], NULL);
    } else {
        // Wraps through Automatic, then every zone in name order.
        int n = (int)tz_db_count + 1;
        int pos = ((st.tz_pick + 1 + steps) % n + n) % n;
        st.tz_pick = pos - 1;
    }
    render();
    return true;
}

// A tap acts on a row directly, as if the knob had selected and pressed it.
static void row_click_cb(lv_event_t *e) {
    int row = (int)(intptr_t)lv_event_get_user_data(e);
    if (row != st.selected && st.editing) {
        edit_end(true);
    }
    select_row(row, true);
    activate();
}

// A drag that takes the selection out of view moves it to the row nearest
// the middle, so the knob carries on from what's on screen.
static void list_scroll_end_cb(lv_event_t *e) {
    (void)e;
    lv_area_t list_area, a;
    lv_obj_get_coords(st.list, &list_area);
    lv_obj_get_coords(st.rows[st.selected], &a);
    if (a.y1 >= list_area.y1 && a.y2 <= list_area.y2) {
        return;
    }
    int32_t mid = (list_area.y1 + list_area.y2) / 2;
    int best = st.selected;
    int32_t best_dist = INT32_MAX;
    for (int i = 0; i < ROW_COUNT; i++) {
        if (!row_visible(i)) {
            continue;
        }
        lv_obj_get_coords(st.rows[i], &a);
        int32_t dist = LV_ABS((a.y1 + a.y2) / 2 - mid);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    select_row(best, false);
}

static void edit_cancel_cb(lv_event_t *e) {
    (void)e;
    edit_end(false);
}

static void edit_apply_cb(lv_event_t *e) {
    (void)e;
    edit_end(true);
}

static lv_obj_t *edit_button_create(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_obj_get_parent(ui_round_button_create(parent, symbol, SET_EDIT_BTN, UI_FONT_SYMBOL_LG, false));
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(btn, SET_EDIT_GAP / 4);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

// Opaque, so the rows it covers don't show through, and clickable, so taps
// beside the buttons don't reach them.
static void edit_bar_create(lv_obj_t *screen) {
    st.edit_bar = lv_obj_create(screen);
    lv_obj_remove_style_all(st.edit_bar);
    lv_obj_remove_flag(st.edit_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(st.edit_bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(st.edit_bar, SET_LIST_W);
    lv_obj_set_style_bg_color(st.edit_bar, lv_obj_get_style_bg_color(screen, 0), 0);
    lv_obj_set_style_bg_opa(st.edit_bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(st.edit_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st.edit_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st.edit_bar, SET_EDIT_GAP, 0);
    edit_button_create(st.edit_bar, LV_SYMBOL_CLOSE, edit_cancel_cb);
    lv_obj_t *apply = edit_button_create(st.edit_bar, LV_SYMBOL_OK, edit_apply_cb);
    lv_obj_set_style_bg_color(apply, lv_color_hex(UI_COLOR_ACCENT), 0);
}

static lv_obj_t *label_create(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *switch_create(lv_obj_t *row) {
    lv_obj_t *track = lv_obj_create(row);
    lv_obj_remove_style_all(track);
    lv_obj_remove_flag(track, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(track, SET_SWITCH_W, SET_SWITCH_H);
    lv_obj_set_style_radius(track, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_t *knob = lv_obj_create(track);
    lv_obj_remove_style_all(knob);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(knob, SET_KNOB, SET_KNOB);
    lv_obj_set_style_radius(knob, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(knob, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    return track;
}

static lv_obj_t *value_create(lv_obj_t *parent, int32_t width) {
    lv_obj_t *value = label_create(parent, UI_FONT_BODY, UI_COLOR_TEXT_DIM);
    lv_obj_set_width(value, width);
    lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    return value;
}

static lv_obj_t *level_create(lv_obj_t *row, int index) {
    lv_obj_t *box = lv_obj_create(row);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, UI_SPACE_S, 0);

    st.level = lv_bar_create(box);
    lv_obj_remove_style_all(st.level);
    lv_obj_remove_flag(st.level, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(st.level, SET_LEVEL_W, 4);
    lv_bar_set_range(st.level, 0, 100);
    lv_obj_set_style_radius(st.level, UI_RADIUS_HAIRLINE, 0);
    lv_obj_set_style_radius(st.level, UI_RADIUS_HAIRLINE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(st.level, lv_color_hex(UI_COLOR_CONTROL), 0);
    lv_obj_set_style_bg_opa(st.level, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(st.level, LV_OPA_COVER, LV_PART_INDICATOR);
    st.values[index] = value_create(box, LV_SIZE_CONTENT);
    return box;
}

static lv_obj_t *row_create(lv_obj_t *list, int index) {
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, lv_pct(100), SET_ROW_H);
    // The selection's highlight.
    lv_obj_set_style_radius(row, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_pad_hor(row, SET_ROW_PAD, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_SPACE_M, 0);
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    st.names[index] = label_create(row, UI_FONT_TITLE, UI_COLOR_TEXT);
    lv_label_set_text(st.names[index], k_rows[index].name);
    switch (k_rows[index].kind) {
    case KIND_SWITCH:
        st.switches[index] = switch_create(row);
        break;
    case KIND_LEVEL:
        level_create(row, index);
        break;
    case KIND_VALUE:
        // Takes whatever the name leaves, truncating with dots.
        st.values[index] = value_create(row, 1);
        lv_obj_set_flex_grow(st.values[index], 1);
        break;
    }
    st.rows[index] = row;
    return row;
}

static lv_obj_t *header_create(lv_obj_t *list, const char *text) {
    lv_obj_t *header = lv_obj_create(list);
    lv_obj_remove_style_all(header);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(header, lv_pct(100), SET_HEADER_H);
    lv_obj_set_style_pad_hor(header, SET_ROW_PAD, 0);
    lv_obj_set_style_pad_bottom(header, UI_SPACE_XS, 0);
    lv_obj_t *label = label_create(header, UI_FONT_OVERLINE, UI_COLOR_TEXT_DIM);
    lv_obj_set_style_text_letter_space(label, UI_OVERLINE_SPACING, 0);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return header;
}

void settings_page_build(lv_obj_t *screen) {
    tool_page_register(screen, &(tool_page_desc_t){.name = "Settings",
                                                   .icon = ICON_SETTINGS,
                                                   .tint = UI_COLOR_TEXT_DIM,
                                                   .rotate = settings_rotate,
                                                   .press = activate});

    lv_obj_t *title = tool_title_create(screen, "SETTINGS", lv_color_hex(UI_COLOR_TEXT_DIM));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, SET_TITLE_Y);

    st.list = lv_obj_create(screen);
    lv_obj_remove_style_all(st.list);
    lv_obj_set_size(st.list, SET_LIST_W, SET_LIST_H);
    lv_obj_set_flex_flow(st.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(st.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(st.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(st.list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_align(st.list, LV_ALIGN_TOP_MID, 0, SET_LIST_TOP);
    lv_obj_add_event_cb(st.list, list_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);
    for (size_t i = 0; i < LAYOUT_COUNT; i++) {
        st.items[i] = k_layout[i].row < 0 ? header_create(st.list, k_layout[i].header)
                                          : row_create(st.list, k_layout[i].row);
    }
    lv_obj_set_flag(st.rows[ROW_AIRPLAY2], LV_OBJ_FLAG_HIDDEN, !row_visible(ROW_AIRPLAY2));
    edit_bar_create(screen);
    lv_obj_add_event_cb(screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    st.selected = ROW_BRIGHTNESS;
    render();
    fade();
    center_row(st.selected, LV_ANIM_OFF);
}
