#include "ui_tools.h"

#include <stdio.h>
#include <string.h>

#include "tz.h"

// A wheel: the selected row always sits in a fixed highlight at the middle
// of the round screen, the list scrolls through it, and rows fade with
// distance so the clipped top and bottom of the circle stay quiet. Rows are
// grouped under section headers the selection skips. The knob moves the
// selection and a press (or a tap on a row) acts on it: switches flip at
// once, brightness and timezone enter an edit mode where the knob changes
// the value until the next press/tap, and the name, web page and (automatic)
// location rows open a QR code for the device's web page, which has every
// setting too (a name or a city can't be typed here).

#define SET_TINT        0x9a9aa4
#define SET_ACCENT      0x4ea1ff
#define SET_DANGER      0xff7a7a
#define SET_SWITCH_OFF  0x3a3a46
#define SET_TITLE_Y     (-134)
#define SET_LIST_W      272
#define SET_LIST_H      236
#define SET_LIST_Y      14
#define SET_ROW_H       40
#define SET_HEADER_H    30
#define SET_ROW_PAD     16
#define SET_SWITCH_W    36
#define SET_SWITCH_H    22
#define SET_KNOB        18
#define SET_LEVEL_W     52
#define SET_FADE_ROWS   3   // rows from the middle to the faintest
#define SET_FADE_MIN    40
#define SET_BRIGHT_STEP 5
#define SET_BRIGHT_MIN  5
// Timezone picker position for "automatic", ahead of tz_db's entries.
#define TZ_AUTO (-1)
#define SET_QR_SIZE     150
#define SET_QR_Y        (-4)
#define SET_QR_TITLE_Y  (-112)
#define SET_QR_HINT_Y   100
#define SET_QR_URL_Y    122

typedef enum {
    ROW_BRIGHTNESS,
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
    [ROW_HAPTICS] = {"Haptics", KIND_SWITCH},
    [ROW_CLOCK] = {"Clock", KIND_VALUE},
    [ROW_LOCATION] = {"Location", KIND_VALUE},
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
    .clock_24h = true,
    .haptics = true,
    .bluetooth = true,
    .airplay = UI_AIRPLAY_1 | UI_AIRPLAY_2,
    .tz_auto = true,
    .location_auto = true,
};

static struct {
    lv_obj_t *list;
    lv_obj_t *highlight;
    lv_obj_t *items[LAYOUT_COUNT];
    lv_obj_t *rows[ROW_COUNT];
    lv_obj_t *names[ROW_COUNT];
    lv_obj_t *values[ROW_COUNT];   // KIND_VALUE and KIND_LEVEL
    lv_obj_t *switches[ROW_COUNT]; // KIND_SWITCH: the track; its child is the knob
    lv_obj_t *level;               // brightness bar
    int selected;
    bool editing;
    bool forget_armed; // the selected row (a Forget one, or a pinned location) pressed once
    lv_obj_t *qr_overlay; // while open
    lv_obj_t *qr;
    lv_obj_t *qr_url;
    int tz_pick; // TZ_AUTO or a tz_db index, while editing the timezone
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

static void location_text(char *out, size_t len) {
    const ui_settings_t *s = &s_settings;
    if (!s->location_auto) {
        snprintf(out, len, "%s", st.forget_armed ? "Press for Auto" : s->location);
    } else {
        snprintf(out, len, s->location[0] ? "Auto \xC2\xB7 %s" : "Auto", s->location);
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

static void bt_forget_text(char *buf, size_t len) {
    const pairing_status_t *bt = bluetooth_status();
    snprintf(buf, len, "%s", bt && bt->bonded && st.forget_armed ? "Press again" : "");
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
    default:
        return false;
    }
}

static void switch_set(lv_obj_t *track, bool on) {
    lv_obj_set_style_bg_color(track, lv_color_hex(on ? SET_ACCENT : SET_SWITCH_OFF), 0);
    lv_obj_align(lv_obj_get_child(track, 0), on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -2 : 2, 0);
}

static void value_text(int row, char *buf, size_t len) {
    const ui_settings_t *s = &s_settings;
    bool editing = st.editing && st.selected == row;
    switch (row) {
    case ROW_BRIGHTNESS:
        snprintf(buf, len, editing ? LV_SYMBOL_LEFT " %u%% " LV_SYMBOL_RIGHT : "%u%%", s->brightness);
        break;
    case ROW_CLOCK:
        snprintf(buf, len, "%s", s->clock_24h ? "24-hour" : "12-hour");
        break;
    case ROW_UNITS:
        snprintf(buf, len, "%s", s->fahrenheit ? "\xC2\xB0" "F" : "\xC2\xB0" "C");
        break;
    case ROW_LOCATION:
        location_text(buf, len);
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
    case ROW_BT_FORGET:
        bt_forget_text(buf, len);
        break;
    case ROW_FORGET:
        snprintf(buf, len, "%s", st.forget_armed ? "Press again" : "");
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
        uint32_t color = i == ROW_FORGET || i == ROW_BT_FORGET ? SET_DANGER : st.editing && st.selected == i ? SET_ACCENT : SET_TINT;
        lv_obj_set_style_text_color(st.values[i], lv_color_hex(color), 0);
    }
    lv_obj_set_style_text_color(st.names[ROW_FORGET], lv_color_hex(SET_DANGER), 0);
    lv_obj_set_style_text_color(st.names[ROW_BT_FORGET], lv_color_hex(SET_DANGER), 0);
    lv_obj_set_style_text_color(st.names[ROW_AIRPLAY1], lv_color_white(), 0);
    ui_label_set_text(st.names[ROW_AIRPLAY1], s_settings.airplay2_available ? "AirPlay 1" : "AirPlay");
    lv_bar_set_value(st.level, s_settings.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(st.level, lv_color_hex(st.editing && st.selected == ROW_BRIGHTNESS ? SET_ACCENT : 0xffffff),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(st.highlight, st.editing ? 56 : 26, 0);
    lv_obj_set_style_bg_color(st.highlight, lv_color_hex(st.editing ? SET_ACCENT : 0xffffff), 0);
}

// Rows fade with their distance from the middle. Labels and switches each
// take their own opacity: a whole-row opa would render every row through a
// layer.
static void fade(void) {
    lv_area_t list_area;
    lv_obj_get_coords(st.list, &list_area);
    int32_t mid = (list_area.y1 + list_area.y2) / 2;
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

// Brings a row to the highlight in the middle.
static void center_row(int row, lv_anim_enable_t anim) {
    lv_obj_update_layout(st.list);
    lv_area_t list_area, row_area;
    lv_obj_get_coords(st.list, &list_area);
    lv_obj_get_coords(st.rows[row], &row_area);
    int32_t offset = (row_area.y1 + row_area.y2) / 2 - (list_area.y1 + list_area.y2) / 2;
    lv_obj_scroll_to_y(st.list, lv_obj_get_scroll_y(st.list) + offset, anim);
    if (!anim) {
        fade();
    }
}

static void select_row(int row, bool scroll) {
    if (row < 0 || row >= ROW_COUNT || !row_visible(row)) {
        return;
    }
    if (row != st.selected) {
        st.selected = row;
        st.forget_armed = false;
        render();
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
    lv_obj_set_width(label, 240);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    return label;
}

static void qr_open(lv_obj_t *screen, const char *title) {
    st.qr_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(st.qr_overlay);
    lv_obj_set_size(st.qr_overlay, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(st.qr_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(st.qr_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(st.qr_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(st.qr_overlay, qr_click_cb, LV_EVENT_CLICKED, NULL);

    lv_label_set_text(qr_label(&inter_semi_16, LV_OPA_COVER, SET_QR_TITLE_Y), title);
    st.qr = lv_qrcode_create(st.qr_overlay);
    lv_qrcode_set_size(st.qr, SET_QR_SIZE);
    lv_qrcode_set_dark_color(st.qr, lv_color_black());
    lv_qrcode_set_light_color(st.qr, lv_color_white());
    lv_qrcode_set_quiet_zone(st.qr, true);
    lv_obj_align(st.qr, LV_ALIGN_CENTER, 0, SET_QR_Y);
    lv_label_set_text(qr_label(&inter_med_13, LV_OPA_COVER, SET_QR_HINT_Y), "Scan on the same WiFi");
    st.qr_url = qr_label(&inter_med_12, LV_OPA_60, SET_QR_URL_Y);
    qr_show_url();
}

// Leaving the page (e.g. for another app) closes the code.
static void screen_unloaded_cb(lv_event_t *e) {
    (void)e;
    qr_close();
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

static void activate(void) {
    const ui_settings_t *s = &s_settings;
    if (st.qr_overlay) {
        qr_close();
        return;
    }
    if (st.editing) {
        st.editing = false;
        if (st.selected == ROW_TIMEZONE) {
            emit(UI_SETTING_TIMEZONE, 0, st.tz_pick == TZ_AUTO ? NULL : tz_db[st.tz_pick].name);
        }
        render();
        return;
    }
    switch (st.selected) {
    case ROW_BRIGHTNESS:
        st.editing = true;
        break;
    case ROW_TIMEZONE:
        st.editing = true;
        st.tz_pick = s->tz_auto ? TZ_AUTO : tz_db_find(s->tz_name);
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
            if (st.forget_armed) {
                ui_pairing_request(UI_PAIRING_FORGET);
            }
            st.forget_armed = !st.forget_armed;
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
        // A city can't be typed here: the web page picks one, and this
        // only goes back to automatic.
        if (!s->location_auto) {
            if (st.forget_armed) {
                emit(UI_SETTING_LOCATION, 0, NULL);
            }
            st.forget_armed = !st.forget_armed;
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
        if (st.forget_armed) {
            emit(UI_SETTING_FORGET_WIFI, 0, NULL);
        }
        st.forget_armed = !st.forget_armed;
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
        activate();
    }
    select_row(row, true);
    activate();
}

static void list_scroll_cb(lv_event_t *e) {
    (void)e;
    fade();
}

// A drag leaves the selection on whichever row it stopped nearest the
// middle; snapping has already centred it.
static void list_scroll_end_cb(lv_event_t *e) {
    (void)e;
    if (lv_obj_is_scrolling(st.list) || st.editing) {
        return;
    }
    lv_area_t list_area;
    lv_obj_get_coords(st.list, &list_area);
    int32_t mid = (list_area.y1 + list_area.y2) / 2;
    int best = st.selected;
    int32_t best_dist = INT32_MAX;
    for (int i = 0; i < ROW_COUNT; i++) {
        if (!row_visible(i)) {
            continue;
        }
        lv_area_t a;
        lv_obj_get_coords(st.rows[i], &a);
        int32_t dist = LV_ABS((a.y1 + a.y2) / 2 - mid);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    select_row(best, false);
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
    lv_obj_set_style_radius(track, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
    lv_obj_t *knob = lv_obj_create(track);
    lv_obj_remove_style_all(knob);
    lv_obj_remove_flag(knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(knob, SET_KNOB, SET_KNOB);
    lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(knob, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(knob, LV_OPA_COVER, 0);
    return track;
}

static lv_obj_t *value_create(lv_obj_t *parent, int32_t width) {
    lv_obj_t *value = label_create(parent, &inter_med_13, SET_TINT);
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
    lv_obj_set_style_pad_column(box, 8, 0);

    st.level = lv_bar_create(box);
    lv_obj_remove_style_all(st.level);
    lv_obj_remove_flag(st.level, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(st.level, SET_LEVEL_W, 4);
    lv_bar_set_range(st.level, 0, 100);
    lv_obj_set_style_radius(st.level, 2, 0);
    lv_obj_set_style_radius(st.level, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(st.level, lv_color_hex(SET_SWITCH_OFF), 0);
    lv_obj_set_style_bg_opa(st.level, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(st.level, LV_OPA_COVER, LV_PART_INDICATOR);
    st.values[index] = value_create(box, LV_SIZE_CONTENT);
    return box;
}

static lv_obj_t *row_create(lv_obj_t *list, int index) {
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_set_size(row, lv_pct(100), SET_ROW_H);
    lv_obj_set_style_pad_hor(row, SET_ROW_PAD, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    st.names[index] = label_create(row, &inter_semi_14, 0xffffff);
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
    lv_obj_set_style_pad_bottom(header, 4, 0);
    lv_obj_t *label = label_create(header, &inter_semi_11, SET_TINT);
    lv_obj_set_style_text_letter_space(label, 1, 0);
    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return header;
}

void settings_page_build(lv_obj_t *screen) {
    tool_page_register(screen, &(tool_page_desc_t){.name = "Settings",
                                                   .icon = ICON_SETTINGS,
                                                   .tint = SET_TINT,
                                                   .rotate = settings_rotate,
                                                   .press = activate});

    lv_obj_t *title = tool_title_create(screen, "SETTINGS", lv_color_hex(SET_TINT));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, SET_TITLE_Y);

    // Fixed in the middle; rows scroll through it.
    st.highlight = lv_obj_create(screen);
    lv_obj_remove_style_all(st.highlight);
    lv_obj_remove_flag(st.highlight, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(st.highlight, SET_LIST_W, SET_ROW_H);
    lv_obj_set_style_radius(st.highlight, SET_ROW_H / 2, 0);
    lv_obj_align(st.highlight, LV_ALIGN_CENTER, 0, SET_LIST_Y);

    st.list = lv_obj_create(screen);
    lv_obj_remove_style_all(st.list);
    lv_obj_set_size(st.list, SET_LIST_W, SET_LIST_H);
    lv_obj_set_flex_flow(st.list, LV_FLEX_FLOW_COLUMN);
    // Room for the first and last rows to reach the middle.
    lv_obj_set_style_pad_ver(st.list, (SET_LIST_H - SET_ROW_H) / 2, 0);
    lv_obj_set_scroll_dir(st.list, LV_DIR_VER);
    lv_obj_set_scroll_snap_y(st.list, LV_SCROLL_SNAP_CENTER);
    lv_obj_set_scrollbar_mode(st.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(st.list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_align(st.list, LV_ALIGN_CENTER, 0, SET_LIST_Y);
    lv_obj_add_event_cb(st.list, list_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(st.list, list_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);
    for (size_t i = 0; i < LAYOUT_COUNT; i++) {
        st.items[i] = k_layout[i].row < 0 ? header_create(st.list, k_layout[i].header)
                                          : row_create(st.list, k_layout[i].row);
    }
    lv_obj_set_flag(st.rows[ROW_AIRPLAY2], LV_OBJ_FLAG_HIDDEN, !row_visible(ROW_AIRPLAY2));
    lv_obj_add_event_cb(screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    st.selected = ROW_BRIGHTNESS;
    render();
    center_row(st.selected, LV_ANIM_OFF);
}
