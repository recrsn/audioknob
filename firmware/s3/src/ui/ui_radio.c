#include "ui_tools.h"

#include <stdio.h>
#include <string.h>

// The station presets as a list under the title, like Settings: the knob
// moves the selection (the knob has no press), a tap plays a station, and a
// tap on the one playing stops it. The station playing is tinted and gets a
// second line: what's on, or what's happening. Stations are added and
// ordered on the device's web page.

#define RADIO_TITLE_Y   UI_TITLE_TOP_Y
#define RADIO_LIST_W    272
#define RADIO_LIST_TOP  (UI_SAFE_TOP + 22)
#define RADIO_LIST_H    (304 - RADIO_LIST_TOP)
#define RADIO_ROW_H     48
#define RADIO_ROW_PAD   UI_SPACE_L
#define RADIO_FADE_ROWS 3
#define RADIO_FADE_MIN  40
#define RADIO_EMPTY_Y   (-8)

typedef struct {
    lv_obj_t *row;
    lv_obj_t *name;
    lv_obj_t *detail;
    lv_obj_t *glyph;
} station_row_t;

static struct {
    lv_obj_t *screen;
    lv_obj_t *list;
    lv_obj_t *empty;
    lv_obj_t *empty_hint;
    station_row_t rows[UI_RADIO_MAX];
    ui_radio_t *data; // the last push, in LVGL's heap
    int built;        // rows that exist
    int selected;
    void (*handler)(int station);
} rd;

void ui_set_radio_handler(void (*handler)(int station)) {
    rd.handler = handler;
}

static bool active(const ui_radio_t *r) {
    return r->current >= 0 && r->state != UI_RADIO_STOPPED && r->state != UI_RADIO_FAILED;
}

static const char *detail_text(const ui_radio_t *r) {
    switch (r->state) {
    case UI_RADIO_PLAYING:
        return r->detail[0] ? r->detail : "Playing";
    case UI_RADIO_CONNECTING:
        return "Connecting\xE2\x80\xA6";
    case UI_RADIO_BUFFERING:
        return "Buffering\xE2\x80\xA6";
    case UI_RADIO_RETRYING:
        return "Reconnecting\xE2\x80\xA6";
    case UI_RADIO_FAILED:
        return r->detail[0] ? r->detail : "Can't play this station";
    case UI_RADIO_STOPPED:
        break;
    }
    return "";
}

// Rows fade with their distance from the selection, per label (a whole-row
// opa would render each row through a layer).
static void fade(void) {
    if (!rd.data->count) {
        return;
    }
    lv_obj_update_layout(rd.list);
    lv_area_t sel;
    lv_obj_get_coords(rd.rows[rd.selected].row, &sel);
    int32_t mid = (sel.y1 + sel.y2) / 2;
    for (int i = 0; i < rd.data->count; i++) {
        lv_area_t a;
        lv_obj_get_coords(rd.rows[i].row, &a);
        int32_t dist = LV_ABS((a.y1 + a.y2) / 2 - mid);
        int32_t opa = LV_OPA_COVER - (LV_OPA_COVER - RADIO_FADE_MIN) * dist / (RADIO_ROW_H * RADIO_FADE_ROWS);
        opa = opa < RADIO_FADE_MIN ? RADIO_FADE_MIN : opa;
        lv_obj_set_style_text_opa(rd.rows[i].name, (lv_opa_t)opa, 0);
        lv_obj_set_style_text_opa(rd.rows[i].detail, (lv_opa_t)opa, 0);
        lv_obj_set_style_text_opa(rd.rows[i].glyph, (lv_opa_t)opa, 0);
    }
}

static void render(void) {
    const ui_radio_t *r = rd.data;
    bool any = r->count > 0;
    ui_set_visible(rd.list, any);
    ui_set_visible(rd.empty, !any);
    ui_set_visible(rd.empty_hint, !any);
    if (!any) {
        const char *host = ui_settings_current()->hostname;
        char hint[96];
        snprintf(hint, sizeof(hint), "Add them on the web page:\nhttp://%s.local", host[0] ? host : "audioknob");
        ui_label_set_text(rd.empty_hint, hint);
        return;
    }
    for (int i = 0; i < r->count; i++) {
        station_row_t *row = &rd.rows[i];
        bool current = i == r->current;
        bool failed = current && r->state == UI_RADIO_FAILED;
        lv_obj_set_style_bg_opa(row->row, i == rd.selected ? UI_OPA_SELECTED : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(row->name, lv_color_hex(current ? UI_TINT_RADIO : UI_COLOR_TEXT), 0);
        ui_set_visible(row->detail, current && r->state != UI_RADIO_STOPPED);
        if (current) {
            ui_label_set_text(row->detail, detail_text(r));
            lv_obj_set_style_text_color(row->detail, lv_color_hex(failed ? UI_COLOR_DANGER : UI_COLOR_TEXT_DIM), 0);
        }
        // The one playing shows what a tap on it does.
        ui_set_visible(row->glyph, current && active(r));
    }
}

static void center_row(int index, lv_anim_enable_t anim) {
    lv_obj_update_layout(rd.list);
    lv_area_t list_area, row_area;
    lv_obj_get_coords(rd.list, &list_area);
    lv_obj_get_coords(rd.rows[index].row, &row_area);
    int32_t y = lv_obj_get_scroll_y(rd.list);
    int32_t target = y + (row_area.y1 + row_area.y2) / 2 - (list_area.y1 + list_area.y2) / 2;
    int32_t max = y + lv_obj_get_scroll_bottom(rd.list);
    target = target < 0 ? 0 : target > max ? max : target;
    lv_obj_scroll_to_y(rd.list, target, anim);
}

static void select_row(int index, bool scroll) {
    if (index < 0 || index >= rd.data->count) {
        return;
    }
    if (index != rd.selected) {
        rd.selected = index;
        render();
        fade();
    }
    if (scroll) {
        center_row(index, LV_ANIM_ON);
    }
}

static void row_click_cb(lv_event_t *e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    ui_radio_t *r = rd.data;
    bool stop = index == r->current && active(r);
    select_row(index, true);
    // Shows at once; the next push confirms it.
    if (stop) {
        r->state = UI_RADIO_STOPPED;
    } else {
        r->current = index;
        r->state = UI_RADIO_CONNECTING;
        r->detail[0] = '\0';
    }
    render();
    if (rd.handler) {
        rd.handler(stop ? -1 : index);
    }
}

// A drag that takes the selection out of view moves it to the row nearest
// the middle, so the knob carries on from what's on screen.
static void list_scroll_end_cb(lv_event_t *e) {
    (void)e;
    lv_area_t list_area, a;
    lv_obj_get_coords(rd.list, &list_area);
    lv_obj_get_coords(rd.rows[rd.selected].row, &a);
    if (a.y1 >= list_area.y1 && a.y2 <= list_area.y2) {
        return;
    }
    int32_t mid = (list_area.y1 + list_area.y2) / 2;
    int best = rd.selected;
    int32_t best_dist = INT32_MAX;
    for (int i = 0; i < rd.data->count; i++) {
        lv_obj_get_coords(rd.rows[i].row, &a);
        int32_t dist = LV_ABS((a.y1 + a.y2) / 2 - mid);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    select_row(best, false);
}

static bool radio_rotate(int steps) {
    if (rd.data->count) {
        int index = rd.selected + steps;
        select_row(index < 0 ? 0 : index >= rd.data->count ? rd.data->count - 1 : index, true);
    }
    return true;
}

static lv_obj_t *label_create(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

// A content-tall label wraps rather than dots, so each is one line tall.
static lv_obj_t *line_create(lv_obj_t *parent, const lv_font_t *font, uint32_t color) {
    lv_obj_t *label = label_create(parent, font, color);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_height(label, lv_font_get_line_height(font));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(label, "");
    return label;
}

static void row_create(int index) {
    station_row_t *r = &rd.rows[index];
    r->row = lv_obj_create(rd.list);
    lv_obj_remove_style_all(r->row);
    lv_obj_remove_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r->row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(r->row, lv_pct(100), RADIO_ROW_H);
    lv_obj_set_style_radius(r->row, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(r->row, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_pad_hor(r->row, RADIO_ROW_PAD, 0);
    lv_obj_set_style_pad_column(r->row, UI_SPACE_M, 0);
    lv_obj_set_flex_flow(r->row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r->row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(r->row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    lv_obj_t *text = lv_obj_create(r->row);
    lv_obj_remove_style_all(text);
    lv_obj_remove_flag(text, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, UI_SPACE_XXS, 0);
    r->name = line_create(text, UI_FONT_TITLE, UI_COLOR_TEXT);
    r->detail = line_create(text, UI_FONT_CAPTION, UI_COLOR_TEXT_DIM);

    r->glyph = label_create(r->row, UI_FONT_SYMBOL_SM, UI_TINT_RADIO);
    lv_label_set_text(r->glyph, LV_SYMBOL_STOP);
}

// Rows are made as stations first need them and kept; extra ones hide.
static void sync_rows(void) {
    const ui_radio_t *r = rd.data;
    while (rd.built < r->count) {
        row_create(rd.built++);
    }
    for (int i = 0; i < rd.built; i++) {
        ui_set_visible(rd.rows[i].row, i < r->count);
        if (i < r->count) {
            ui_label_set_text(rd.rows[i].name, r->names[i]);
        }
    }
}

void ui_update_radio(const ui_radio_t *radio) {
    bool rows_changed = radio->count != rd.data->count ||
                        memcmp(radio->names, rd.data->names, sizeof(radio->names[0]) * (size_t)radio->count);
    *rd.data = *radio;
    if (rd.selected >= radio->count) {
        rd.selected = radio->count ? radio->count - 1 : 0;
    }
    if (rows_changed) {
        sync_rows();
    }
    render();
    if (rows_changed) {
        fade();
    }
}

// Coming back to the page, the selection starts on the station playing.
static void screen_load_cb(lv_event_t *e) {
    (void)e;
    if (rd.data->current >= 0) {
        rd.selected = rd.data->current;
    }
    render();
    if (rd.data->count) {
        fade();
        center_row(rd.selected, LV_ANIM_OFF);
    }
}

void radio_page_build(lv_obj_t *screen) {
    tool_page_register(screen, &(tool_page_desc_t){
                                   .name = "Radio", .icon = ICON_RADIO, .tint = UI_TINT_RADIO, .rotate = radio_rotate});
    rd.screen = screen;
    rd.data = lv_malloc_zeroed(sizeof(*rd.data));
    LV_ASSERT_MALLOC(rd.data);
    rd.data->current = -1;

    lv_obj_t *title = tool_title_create(screen, "RADIO", lv_color_hex(UI_TINT_RADIO));
    lv_obj_align(title, LV_ALIGN_CENTER, 0, RADIO_TITLE_Y);

    rd.list = lv_obj_create(screen);
    lv_obj_remove_style_all(rd.list);
    lv_obj_set_size(rd.list, RADIO_LIST_W, RADIO_LIST_H);
    lv_obj_set_flex_flow(rd.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(rd.list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(rd.list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(rd.list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_align(rd.list, LV_ALIGN_TOP_MID, 0, RADIO_LIST_TOP);
    lv_obj_add_event_cb(rd.list, list_scroll_end_cb, LV_EVENT_SCROLL_END, NULL);

    rd.empty = label_create(screen, UI_FONT_TITLE, UI_COLOR_TEXT);
    lv_label_set_text(rd.empty, "No stations yet");
    lv_obj_align(rd.empty, LV_ALIGN_CENTER, 0, RADIO_EMPTY_Y);
    rd.empty_hint = label_create(screen, UI_FONT_BODY, UI_COLOR_TEXT_DIM);
    lv_obj_set_width(rd.empty_hint, UI_TEXT_W);
    lv_obj_set_style_text_align(rd.empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(rd.empty_hint, rd.empty, LV_ALIGN_OUT_BOTTOM_MID, 0, UI_SPACE_S);

    lv_obj_add_event_cb(screen, screen_load_cb, LV_EVENT_SCREEN_LOAD_START, NULL);
    render();
}
