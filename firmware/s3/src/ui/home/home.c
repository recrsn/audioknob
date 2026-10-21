#include "ui/home/home.h"

#include <stdio.h>
#include <time.h>

#include "ui/assets/fonts.h"
#include "ui/assets/icons.h"
#include "ui/home/band.h"
#include "ui/home/np_card.h"
#include "ui/shell/nav.h"
#include "ui/widgets/widgets.h"

#define CLOCK_CHIP_GAP UI_SPACE_S

static struct {
    lv_obj_t *screen;
    lv_obj_t *time_label;
    lv_obj_t *ampm_label;
    lv_obj_t *date_label;
    lv_obj_t *weather;
    lv_obj_t *weather_icon;
    lv_obj_t *weather_temp;
    lv_obj_t *weather_sky;
    // The weather's second line, per the Settings page's "Show air
    // quality": per reading, a glyph tinted by severity, then the value.
    lv_obj_t *weather_air_row;
    lv_obj_t *weather_air_icon[2];
    lv_obj_t *weather_air[2];
    // One per app, shown while that app has something in progress; Talk
    // and the inbox show in the band instead.
    lv_obj_t *chips[UI_APP_COUNT];
    lv_obj_t *chip_labels[UI_APP_COUNT];
} idle_screen;

static char s_time[8];

lv_obj_t *home_screen(void) {
    return idle_screen.screen;
}

const char *home_time_text(void) {
    return s_time;
}

static void clock_tap_cb(lv_event_t *e) {
    (void)e;
    nav_open_launcher();
}

static void clock_chip_cb(lv_event_t *e) {
    nav_open_app((ui_app_t)(intptr_t)lv_event_get_user_data(e));
}

static void clock_weather_cb(lv_event_t *e) {
    (void)e;
    nav_open_app(UI_APP_WEATHER);
}

// Under the time: conditions glyph, temperature and sky, then air quality
// and UV when they matter, opening the weather page on tap.
static void weather_complication_build(lv_obj_t *col) {
    lv_obj_t *comp = lv_obj_create(col);
    lv_obj_remove_style_all(comp);
    lv_obj_remove_flag(comp, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(comp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(comp, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(comp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(comp, UI_SPACE_XS, 0);
    lv_obj_set_style_margin_top(comp, HOME_COMP_GAP, 0);
    lv_obj_add_flag(comp, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_ext_click_area(comp, UI_TOUCH_EXT);
    lv_obj_add_event_cb(comp, clock_weather_cb, LV_EVENT_CLICKED, NULL);
    idle_screen.weather = comp;

    lv_obj_t *now = ui_flex_row_create(comp, UI_SPACE_S, 0);
    lv_obj_remove_flag(now, LV_OBJ_FLAG_CLICKABLE);
    idle_screen.weather_icon = lv_label_create(now);
    lv_obj_set_style_text_font(idle_screen.weather_icon, UI_FONT_ICON_SM, 0);
    lv_obj_set_style_text_color(idle_screen.weather_icon, lv_color_hex(UI_TINT_WEATHER), 0);
    lv_label_set_text(idle_screen.weather_icon, ICON_CLOUD);
    idle_screen.weather_temp = lv_label_create(now);
    lv_obj_set_style_text_font(idle_screen.weather_temp, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(idle_screen.weather_temp, lv_color_hex(UI_COLOR_TEXT), 0);
    idle_screen.weather_sky = lv_label_create(now);
    lv_obj_set_style_text_font(idle_screen.weather_sky, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(idle_screen.weather_sky, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_opa(idle_screen.weather_sky, UI_OPA_TEXT_MUTED, 0);

    lv_obj_t *air = ui_flex_row_create(comp, UI_SPACE_S, 0);
    lv_obj_remove_flag(air, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(air, LV_OBJ_FLAG_HIDDEN);
    idle_screen.weather_air_row = air;
    static const char *const k_air_icons[2] = {ICON_AIR, ICON_UV};
    for (int i = 0; i < 2; i++) {
        lv_obj_t *icon = lv_label_create(air);
        lv_obj_set_style_text_font(icon, UI_FONT_ICON_SM, 0);
        lv_obj_set_style_margin_left(icon, i ? UI_SPACE_XS : 0, 0);
        lv_label_set_text(icon, k_air_icons[i]);
        idle_screen.weather_air_icon[i] = icon;
        idle_screen.weather_air[i] = lv_label_create(air);
        lv_obj_set_style_text_font(idle_screen.weather_air[i], UI_FONT_LABEL, 0);
        lv_obj_set_style_text_color(idle_screen.weather_air[i], lv_color_hex(UI_COLOR_TEXT), 0);
    }
}

void home_build(void) {
    idle_screen.screen = ui_screen_create();
    lv_obj_add_event_cb(idle_screen.screen, clock_tap_cb, LV_EVENT_CLICKED, NULL);

    ui_wallpaper_create(idle_screen.screen);

    // Everything stacks in one centered column (mock's .clock-idle), so the
    // weather pill can show or hide without re-pinning the clock.
    lv_obj_t *col = lv_obj_create(idle_screen.screen);
    lv_obj_remove_style_all(col);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(col, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, UI_SPACE_XS, 0);
    ui_touch_pad(col, UI_TOUCH_EXT);
    lv_obj_center(col);

    idle_screen.date_label = ui_glance_label_create(col, UI_FONT_TITLE, UI_OPA_TEXT_SOFT);

    // Content-wide so the time stays centered; AM/PM hangs off its right
    // edge on the digits' baseline, outside the column's layout.
    lv_obj_t *time = lv_label_create(col);
    lv_obj_set_style_text_font(time, UI_FONT_CLOCK, 0);
    lv_obj_set_style_text_color(time, lv_color_hex(UI_COLOR_TEXT), 0);
    idle_screen.time_label = time;
    lv_obj_t *ampm = lv_label_create(col);
    lv_obj_add_flag(ampm, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_text_font(ampm, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(ampm, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_opa(ampm, UI_OPA_TEXT_SOFT, 0);
    lv_obj_add_flag(ampm, LV_OBJ_FLAG_HIDDEN);
    idle_screen.ampm_label = ampm;

    weather_complication_build(col);
    np_card_build(col);

    band_build(idle_screen.screen);

    lv_obj_t *chips = ui_flex_row_create(idle_screen.screen, CLOCK_CHIP_GAP, 0);
    lv_obj_remove_flag(chips, LV_OBJ_FLAG_CLICKABLE);
    ui_touch_pad(chips, UI_TOUCH_EXT);
    lv_obj_set_style_max_width(chips, UI_CHIP_MAX_W + 2 * UI_TOUCH_EXT, 0);
    lv_obj_align(chips, LV_ALIGN_BOTTOM_MID, 0, -UI_CHIP_INSET + UI_TOUCH_EXT);
    for (int i = 0; i < UI_APP_COUNT; i++) {
        ui_app_info_t info = ui_tools_info((ui_app_t)i);
        idle_screen.chip_labels[i] = ui_glyph_pill_create(chips, info.icon, info.tint, clock_chip_cb, (void *)(intptr_t)i);
        idle_screen.chips[i] = lv_obj_get_parent(idle_screen.chip_labels[i]);
        lv_obj_add_flag(idle_screen.chips[i], LV_OBJ_FLAG_HIDDEN);
    }

    ui_update_clock(false, NULL);
}

// Out of the column's layout, so it follows the time by hand: after the
// time's text changes, and whenever the column reflows around it.
static void ampm_place(void) {
    if (lv_obj_has_flag(idle_screen.ampm_label, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    lv_obj_update_layout(idle_screen.time_label);
    lv_obj_align_to(idle_screen.ampm_label, idle_screen.time_label, LV_ALIGN_OUT_RIGHT_BOTTOM, UI_SPACE_XS,
                    -(UI_FONT_CLOCK->base_line - UI_FONT_TITLE->base_line));
}

// Air quality and UV under the weather when they matter (or always, per
// settings), each a glyph in its severity's color and the reading.
static void weather_air_refresh(bool have_weather) {
    ui_air_reading_t r[2];
    ui_air_readings(&r[0], &r[1]);
    uint8_t mode = ui_settings_current()->air_show;
    bool show[2];
    for (int i = 0; i < 2; i++) {
        show[i] = have_weather && r[i].known && (mode == UI_AIR_SHOW_ALWAYS || (mode == UI_AIR_SHOW_HIGH && r[i].high));
        ui_set_visible(idle_screen.weather_air_icon[i], show[i]);
        ui_set_visible(idle_screen.weather_air[i], show[i]);
        if (!show[i]) {
            continue;
        }
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", r[i].value);
        ui_label_set_text(idle_screen.weather_air[i], buf);
        lv_obj_set_style_text_color(idle_screen.weather_air_icon[i],
                                    lv_color_hex(r[i].high ? r[i].color : UI_TINT_WEATHER), 0);
    }
    ui_set_visible(idle_screen.weather_air_row, show[0] || show[1]);
}

void home_refresh(void) {
    char buf[16];
    bool busy[UI_APP_COUNT];
    int count = 0;
    for (int i = 0; i < UI_APP_COUNT; i++) {
        busy[i] = !band_owns((ui_app_t)i) && ui_tools_status((ui_app_t)i, buf, sizeof(buf));
        count += busy[i];
        ui_set_visible(idle_screen.chips[i], busy[i]);
        if (busy[i]) {
            ui_label_set_text(idle_screen.chip_labels[i], buf);
        }
    }
    // Two chips with their times don't fit the band at the round edge, so
    // more than one shows icons only.
    for (int i = 0; i < UI_APP_COUNT; i++) {
        ui_set_visible(idle_screen.chip_labels[i], busy[i] && count == 1);
    }
    ui_weather_glance_t wx;
    bool have_weather = ui_weather_glance(&wx);
    ui_set_visible(idle_screen.weather, have_weather);
    if (have_weather) {
        ui_label_set_text(idle_screen.weather_icon, wx.icon);
        snprintf(buf, sizeof(buf), "%d\xC2\xB0", wx.temp);
        ui_label_set_text(idle_screen.weather_temp, buf);
        ui_label_set_text(idle_screen.weather_sky, wx.sky);
    }
    weather_air_refresh(have_weather);
    band_refresh();
    ampm_place();
}

void ui_update_clock(bool synced, const struct tm *local_time) {
    if (!synced || !local_time) {
        s_time[0] = '\0';
        ui_label_set_text(idle_screen.time_label, "--:--");
        ui_set_visible(idle_screen.ampm_label, false);
        ui_label_set_text(idle_screen.date_label, "Waiting for time sync");
        return;
    }

    bool h24 = ui_settings_current()->clock_24h;
    int hour = h24 ? local_time->tm_hour : (local_time->tm_hour % 12 ? local_time->tm_hour % 12 : 12);
    char buf[32];
    snprintf(buf, sizeof(buf), h24 ? "%02d:%02d" : "%d:%02d", hour, local_time->tm_min);
    ui_label_set_text(idle_screen.time_label, buf);
    snprintf(s_time, sizeof(s_time), "%s", buf);
    // The clock font has digits and ':' only, so AM/PM is its own label.
    ui_set_visible(idle_screen.ampm_label, !h24);
    if (!h24) {
        ui_label_set_text(idle_screen.ampm_label, local_time->tm_hour < 12 ? "AM" : "PM");
        ampm_place();
    }

    // "Sun 4 Oct" like the mock; %d would zero-pad and %-d isn't portable.
    char weekday[8], month[8];
    strftime(weekday, sizeof(weekday), "%a", local_time);
    strftime(month, sizeof(month), "%b", local_time);
    snprintf(buf, sizeof(buf), "%s %d %s", weekday, local_time->tm_mday, month);
    ui_label_set_text(idle_screen.date_label, buf);
}
