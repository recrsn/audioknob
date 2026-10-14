#include "ui_tools.h"

#include <stdio.h>
#include <string.h>

#include "tz.h"

// A vertical list. The knob moves the highlight and a press (or a tap on a
// row) acts on it: toggles flip at once, brightness and timezone enter an
// edit mode where the knob changes the value until the next press/tap, and
// the name opens a QR code for the device's web page, since a name can't
// be typed here.

#define SET_TINT        0x9a9aa4
#define SET_ACCENT      0x4ea1ff
#define SET_TITLE_Y     (-118)
#define SET_LIST_W      250
#define SET_LIST_H      212
#define SET_LIST_Y      6
#define SET_ROW_PAD     8
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
    ROW_CLOCK,
    ROW_UNITS,
    ROW_TIMEZONE,
    ROW_HAPTICS,
    ROW_BLUETOOTH,
    ROW_AIRPLAY,
    ROW_NAME,
    ROW_WIFI,
    ROW_FORGET,
    ROW_ABOUT,
    ROW_COUNT,
} row_t;

static const char *const k_row_names[ROW_COUNT] = {
    [ROW_BRIGHTNESS] = "Brightness", [ROW_CLOCK] = "Clock",   [ROW_UNITS] = "Units",
    [ROW_TIMEZONE] = "Timezone",     [ROW_HAPTICS] = "Haptics", [ROW_BLUETOOTH] = "Bluetooth",
    [ROW_AIRPLAY] = "AirPlay",       [ROW_NAME] = "Name",       [ROW_WIFI] = "WiFi",
    [ROW_FORGET] = "Forget network", [ROW_ABOUT] = "About",
};

static ui_settings_t s_settings = {
    .brightness = 80,
    .clock_24h = true,
    .haptics = true,
    .bluetooth = true,
    .airplay = UI_AIRPLAY_1 | UI_AIRPLAY_2,
    .tz_auto = true,
};

static struct {
    lv_obj_t *list;
    lv_obj_t *rows[ROW_COUNT];
    lv_obj_t *values[ROW_COUNT];
    int selected;
    bool editing;
    bool forget_armed;
    lv_obj_t *rename;    // QR overlay, while open
    lv_obj_t *rename_qr;
    lv_obj_t *rename_url;
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
            snprintf(out, len, LV_SYMBOL_LEFT "  Automatic  " LV_SYMBOL_RIGHT);
        } else {
            snprintf(out, len, LV_SYMBOL_LEFT "  %s  " LV_SYMBOL_RIGHT, tz_db[st.tz_pick].name);
        }
        return;
    }
    zone_city(s_settings.tz_name, city, sizeof(city));
    if (s_settings.tz_auto) {
        snprintf(out, len, s_settings.tz_name[0] ? "Automatic \xC2\xB7 %s" : "Automatic \xC2\xB7 detecting", city);
    } else {
        snprintf(out, len, "%s", s_settings.tz_name);
    }
}

static const char *airplay_text(const ui_settings_t *s) {
    if (!s->airplay2_available) {
        return s->airplay & UI_AIRPLAY_1 ? "On" : "Off";
    }
    switch (s->airplay & (UI_AIRPLAY_1 | UI_AIRPLAY_2)) {
    case UI_AIRPLAY_1 | UI_AIRPLAY_2:
        return "AirPlay 2 and 1";
    case UI_AIRPLAY_2:
        return "AirPlay 2 only";
    case UI_AIRPLAY_1:
        return "AirPlay 1 only";
    default:
        return "Off";
    }
}

// Each press steps down from both versions to off, then back to both.
static uint8_t airplay_next(const ui_settings_t *s) {
    if (!s->airplay2_available) {
        return s->airplay & UI_AIRPLAY_1 ? 0 : UI_AIRPLAY_1 | UI_AIRPLAY_2;
    }
    switch (s->airplay & (UI_AIRPLAY_1 | UI_AIRPLAY_2)) {
    case UI_AIRPLAY_1 | UI_AIRPLAY_2:
        return UI_AIRPLAY_2;
    case UI_AIRPLAY_2:
        return UI_AIRPLAY_1;
    case UI_AIRPLAY_1:
        return 0;
    default:
        return UI_AIRPLAY_1 | UI_AIRPLAY_2;
    }
}

static void render(void) {
    const ui_settings_t *s = &s_settings;
    char buf[64];
    for (int i = 0; i < ROW_COUNT; i++) {
        switch (i) {
        case ROW_BRIGHTNESS:
            snprintf(buf, sizeof(buf),
                     st.editing && st.selected == i ? LV_SYMBOL_LEFT "  %u%%  " LV_SYMBOL_RIGHT : "%u%%",
                     s->brightness);
            break;
        case ROW_CLOCK:
            snprintf(buf, sizeof(buf), "%s", s->clock_24h ? "24-hour" : "12-hour");
            break;
        case ROW_UNITS:
            snprintf(buf, sizeof(buf), "%s", s->fahrenheit ? "\xC2\xB0" "F" : "\xC2\xB0" "C");
            break;
        case ROW_TIMEZONE:
            timezone_text(buf, sizeof(buf));
            break;
        case ROW_HAPTICS:
            snprintf(buf, sizeof(buf), "%s", s->haptics ? "On" : "Off");
            break;
        case ROW_BLUETOOTH:
            snprintf(buf, sizeof(buf), "%s", s->bluetooth ? "On" : "Off");
            break;
        case ROW_AIRPLAY:
            snprintf(buf, sizeof(buf), "%s", airplay_text(s));
            break;
        case ROW_NAME:
            snprintf(buf, sizeof(buf), "%s", s->device_name);
            break;
        case ROW_WIFI:
            if (s->wifi_ssid[0]) {
                snprintf(buf, sizeof(buf), "%s \xC2\xB7 %s", s->wifi_ssid, s->ip);
            } else {
                snprintf(buf, sizeof(buf), "Not connected");
            }
            break;
        case ROW_FORGET:
            snprintf(buf, sizeof(buf), "%s", st.forget_armed ? "Press again to forget" : "Back to setup QR");
            break;
        case ROW_ABOUT:
            snprintf(buf, sizeof(buf), "%s \xC2\xB7 %lu K / %lu K free", s->version[0] ? s->version : "dev",
                     (unsigned long)s->free_internal_kb, (unsigned long)s->free_psram_kb);
            break;
        }
        ui_label_set_text(st.values[i], buf);
        lv_obj_set_style_text_color(st.values[i], lv_color_hex(st.editing && st.selected == i ? SET_ACCENT : SET_TINT),
                                    0);
    }
}

// The device page by IP: .local names don't resolve on every phone.
static void rename_show_url(void) {
    char url[32];
    snprintf(url, sizeof(url), "http://%s/", s_settings.ip);
    lv_qrcode_update(st.rename_qr, url, strlen(url));
    lv_label_set_text_fmt(st.rename_url, "%s.local", s_settings.hostname);
}

static void rename_close(void) {
    if (st.rename) {
        lv_obj_delete(st.rename);
        st.rename = NULL;
    }
}

static void rename_click_cb(lv_event_t *e) {
    (void)e;
    rename_close();
}

static lv_obj_t *rename_label(const lv_font_t *font, lv_opa_t opa, int32_t y) {
    lv_obj_t *label = lv_label_create(st.rename);
    lv_obj_set_width(label, 240);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    return label;
}

static void rename_open(lv_obj_t *screen) {
    st.rename = lv_obj_create(screen);
    lv_obj_remove_style_all(st.rename);
    lv_obj_set_size(st.rename, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(st.rename, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(st.rename, LV_OPA_COVER, 0);
    lv_obj_add_flag(st.rename, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(st.rename, rename_click_cb, LV_EVENT_CLICKED, NULL);

    lv_label_set_text(rename_label(&inter_semi_16, LV_OPA_COVER, SET_QR_TITLE_Y), "Rename");
    st.rename_qr = lv_qrcode_create(st.rename);
    lv_qrcode_set_size(st.rename_qr, SET_QR_SIZE);
    lv_qrcode_set_dark_color(st.rename_qr, lv_color_black());
    lv_qrcode_set_light_color(st.rename_qr, lv_color_white());
    lv_qrcode_set_quiet_zone(st.rename_qr, true);
    lv_obj_align(st.rename_qr, LV_ALIGN_CENTER, 0, SET_QR_Y);
    lv_label_set_text(rename_label(&inter_med_13, LV_OPA_COVER, SET_QR_HINT_Y), "Scan on the same WiFi");
    st.rename_url = rename_label(&inter_med_12, LV_OPA_60, SET_QR_URL_Y);
    rename_show_url();
}

// Leaving the page (e.g. for another app) closes the code.
static void screen_unloaded_cb(lv_event_t *e) {
    (void)e;
    rename_close();
}

void ui_update_settings(const ui_settings_t *settings) {
    if (!memcmp(settings, &s_settings, sizeof(s_settings))) {
        return;
    }
    bool url_changed = strcmp(settings->ip, s_settings.ip) || strcmp(settings->hostname, s_settings.hostname);
    s_settings = *settings;
    render();
    if (st.rename && url_changed) {
        // Dropped off WiFi: the code would point nowhere.
        if (!s_settings.ip[0]) {
            rename_close();
        } else {
            rename_show_url();
        }
    }
}

static void select_row(int row) {
    if (row < 0 || row >= ROW_COUNT || row == st.selected) {
        return;
    }
    lv_obj_set_style_bg_opa(st.rows[st.selected], LV_OPA_TRANSP, 0);
    st.selected = row;
    st.forget_armed = false;
    lv_obj_set_style_bg_opa(st.rows[row], 26, 0);
    lv_obj_scroll_to_view(st.rows[row], LV_ANIM_ON);
    render();
}

static void activate(void) {
    const ui_settings_t *s = &s_settings;
    if (st.rename) {
        rename_close();
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
    case ROW_AIRPLAY:
        emit(UI_SETTING_AIRPLAY, airplay_next(s), NULL);
        break;
    case ROW_NAME:
        if (s->ip[0]) {
            rename_open(lv_obj_get_screen(st.list));
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
    if (st.rename) {
        return true;
    }
    if (!st.editing) {
        int row = st.selected + steps;
        select_row(row < 0 ? 0 : row >= ROW_COUNT ? ROW_COUNT - 1 : row);
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
    if (row != st.selected) {
        if (st.editing) {
            activate();
        }
        select_row(row);
    }
    activate();
}

static lv_obj_t *row_create(lv_obj_t *list, int index) {
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(row, SET_ROW_PAD, 0);
    lv_obj_set_style_pad_row(row, 2, 0);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, k_row_names[index]);
    lv_obj_set_style_text_font(name, &inter_semi_14, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_width(value, SET_LIST_W - 24);
    lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(value, &inter_med_12, 0);
    st.values[index] = value;
    return row;
}

void settings_page_build(lv_obj_t *screen) {
    tool_page_register(screen, &(tool_page_desc_t){.name = "Settings",
                                                   .icon = ICON_SETTINGS,
                                                   .tint = SET_TINT,
                                                   .rotate = settings_rotate,
                                                   .press = activate});

    lv_obj_t *title = tool_title_create(screen, "SETTINGS", lv_color_hex(SET_TINT));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, SET_TITLE_Y);

    st.list = lv_obj_create(screen);
    lv_obj_remove_style_all(st.list);
    lv_obj_set_size(st.list, SET_LIST_W, SET_LIST_H);
    lv_obj_set_flex_flow(st.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(st.list, 2, 0);
    lv_obj_set_scroll_dir(st.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(st.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_align(st.list, LV_ALIGN_CENTER, 0, SET_LIST_Y);
    for (int i = 0; i < ROW_COUNT; i++) {
        st.rows[i] = row_create(st.list, i);
    }
    lv_obj_set_style_bg_opa(st.rows[0], 26, 0);
    lv_obj_add_event_cb(screen, screen_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);

    render();
}
