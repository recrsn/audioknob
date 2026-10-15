#include "ui_tools.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define WX_TITLE_Y    TOOL_TITLE_TOP_Y
#define WX_NOW_Y      (-52)
#define WX_SKY_Y      (-2)
#define WX_RANGE_Y    20
#define WX_HOURS_Y    74
#define WX_HOUR_W     52
#define WX_HOUR_GAP   8
#define WX_DIM        0x9a9aa4
#define WX_ICON_TINT  0xffc35a

static struct {
    lv_obj_t *title;
    lv_obj_t *now_row;
    lv_obj_t *icon;
    lv_obj_t *temp;
    lv_obj_t *sky;
    lv_obj_t *range;
    lv_obj_t *hours_row;
    lv_obj_t *hour_time[UI_WEATHER_HOURS];
    lv_obj_t *hour_icon[UI_WEATHER_HOURS];
    lv_obj_t *hour_temp[UI_WEATHER_HOURS];
    lv_obj_t *status;
    ui_weather_t data;
    bool rendered_fahrenheit;
    bool rendered_24h;
} wx;

// WMO weather interpretation codes (Open-Meteo's `weather_code`).
static const char *sky_text(uint8_t code) {
    switch (code) {
    case 0: return "Clear";
    case 1: return "Mostly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: case 63: case 65: return "Rain";
    case 66: case 67: return "Freezing rain";
    case 71: case 73: case 75: return "Snow";
    case 77: return "Snow grains";
    case 80: case 81: case 82: return "Showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm, hail";
    default: return "";
    }
}

static const char *sky_icon(uint8_t code, bool is_day) {
    if (code <= 1) {
        return is_day ? ICON_CLEAR_DAY : ICON_CLEAR_NIGHT;
    }
    if (code == 2) {
        return is_day ? ICON_PARTLY_DAY : ICON_PARTLY_NIGHT;
    }
    if (code == 3) {
        return ICON_CLOUD;
    }
    if (code == 45 || code == 48) {
        return ICON_FOG;
    }
    if (code >= 51 && code <= 57) {
        return ICON_DRIZZLE;
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return ICON_SNOW;
    }
    if (code >= 95) {
        return ICON_STORM;
    }
    return ICON_RAIN;
}

static int display_temp(float c) {
    return (int)lroundf(ui_settings_current()->fahrenheit ? c * 9.0f / 5.0f + 32.0f : c);
}

static void format_hour(uint8_t hour, char *out, size_t len) {
    if (ui_settings_current()->clock_24h) {
        snprintf(out, len, "%02u:00", hour);
    } else {
        snprintf(out, len, "%u %s", hour % 12 ? hour % 12 : 12, hour < 12 ? "AM" : "PM");
    }
}

static void render(void) {
    const ui_weather_t *w = &wx.data;
    wx.rendered_fahrenheit = ui_settings_current()->fahrenheit;
    wx.rendered_24h = ui_settings_current()->clock_24h;

    ui_set_visible(wx.now_row, w->valid);
    ui_set_visible(wx.sky, w->valid);
    ui_set_visible(wx.range, w->valid);
    ui_set_visible(wx.hours_row, w->valid);
    ui_set_visible(wx.status, !w->valid);

    char buf[48];
    if (w->place[0]) {
        size_t i = 0;
        for (; w->place[i] && i < sizeof(buf) - 1; i++) {
            char c = w->place[i];
            buf[i] = (c >= 'a' && c <= 'z') ? c - 'a' + 'A' : c;
        }
        buf[i] = '\0';
        ui_label_set_text(wx.title, buf);
    } else {
        ui_label_set_text(wx.title, "WEATHER");
    }

    if (!w->valid) {
        ui_label_set_text(wx.status, w->status[0] ? w->status : "No weather yet");
        return;
    }

    ui_label_set_text(wx.icon, sky_icon(w->code, w->is_day));
    snprintf(buf, sizeof(buf), "%d\xC2\xB0", display_temp(w->temp_c));
    ui_label_set_text(wx.temp, buf);
    ui_label_set_text(wx.sky, sky_text(w->code));
    snprintf(buf, sizeof(buf), "H %d\xC2\xB0   L %d\xC2\xB0", display_temp(w->high_c), display_temp(w->low_c));
    ui_label_set_text(wx.range, buf);

    for (int i = 0; i < UI_WEATHER_HOURS; i++) {
        format_hour(w->hours[i].hour, buf, sizeof(buf));
        ui_label_set_text(wx.hour_time[i], buf);
        ui_label_set_text(wx.hour_icon[i], sky_icon(w->hours[i].code, w->hours[i].is_day));
        snprintf(buf, sizeof(buf), "%d\xC2\xB0", display_temp(w->hours[i].temp_c));
        ui_label_set_text(wx.hour_temp[i], buf);
    }
}

void ui_update_weather(const ui_weather_t *weather) {
    wx.data = *weather;
    render();
}

bool ui_weather_glance(ui_weather_glance_t *out) {
    if (!wx.data.valid) {
        return false;
    }
    out->icon = sky_icon(wx.data.code, wx.data.is_day);
    out->sky = sky_text(wx.data.code);
    out->temp = display_temp(wx.data.temp_c);
    out->high = display_temp(wx.data.high_c);
    out->low = display_temp(wx.data.low_c);
    return true;
}

// Units and clock format live in settings; re-render when they flip.
static void tick(void) {
    const ui_settings_t *s = ui_settings_current();
    if (s->fahrenheit != wx.rendered_fahrenheit || s->clock_24h != wx.rendered_24h) {
        render();
    }
}

static lv_obj_t *label_create(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

static lv_obj_t *row_create(lv_obj_t *parent, lv_flex_flow_t flow, int32_t gap, int32_t y) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, flow);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, gap, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, y);
    return row;
}

void weather_page_build(lv_obj_t *screen) {
    tool_page_register(screen, &(tool_page_desc_t){
                                   .name = "Weather", .icon = ICON_PARTLY_DAY, .tint = WX_ICON_TINT, .tick = tick});

    wx.title = tool_title_create(screen, "WEATHER", lv_color_hex(WX_DIM));
    lv_obj_align(wx.title, LV_ALIGN_CENTER, 0, WX_TITLE_Y);

    wx.now_row = row_create(screen, LV_FLEX_FLOW_ROW, 10, WX_NOW_Y);
    wx.icon = label_create(wx.now_row, &icons_48, WX_ICON_TINT);
    wx.temp = tool_time_label_create(wx.now_row);

    wx.sky = label_create(screen, &inter_semi_16, 0xffffff);
    lv_obj_align(wx.sky, LV_ALIGN_CENTER, 0, WX_SKY_Y);
    wx.range = label_create(screen, &inter_med_13, WX_DIM);
    lv_obj_align(wx.range, LV_ALIGN_CENTER, 0, WX_RANGE_Y);

    wx.hours_row = row_create(screen, LV_FLEX_FLOW_ROW, WX_HOUR_GAP, WX_HOURS_Y);
    for (int i = 0; i < UI_WEATHER_HOURS; i++) {
        lv_obj_t *col = row_create(wx.hours_row, LV_FLEX_FLOW_COLUMN, 4, 0);
        lv_obj_set_width(col, WX_HOUR_W);
        wx.hour_time[i] = label_create(col, &inter_med_12, WX_DIM);
        wx.hour_icon[i] = label_create(col, &icons_20, WX_ICON_TINT);
        wx.hour_temp[i] = label_create(col, &inter_semi_13, 0xffffff);
    }

    wx.status = label_create(screen, &inter_med_13, WX_DIM);
    lv_obj_set_width(wx.status, 220);
    lv_obj_set_style_text_align(wx.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(wx.status, LV_ALIGN_CENTER, 0, 0);

    ui_weather_t none = {.valid = false};
    snprintf(none.status, sizeof(none.status), "Waiting for location");
    ui_update_weather(&none);
}
