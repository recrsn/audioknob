#include "ui/now_playing/switch.h"

#include "ui/assets/fonts.h"
#include "ui/now_playing/now_playing.h"
#include "ui/shell/nav.h"
#include "ui/widgets/widgets.h"

// The switch screen offers the sources a phone streams from; the radio is
// an app, played from its own page.
#define SWITCH_SOURCES 2

#define SWITCH_LIST_Y      4
#define SWITCH_ITEM_W      220
#define SWITCH_ITEM_GAP    UI_SPACE_M

static struct {
    lv_obj_t *screen;
    lv_obj_t *items[SWITCH_SOURCES]; // indexed by ui_source_t
} s_switch;

static void (*s_source_handler)(ui_source_t);

void ui_set_source_handler(void (*handler)(ui_source_t source)) {
    s_source_handler = handler;
}

lv_obj_t *switch_screen(void) {
    return s_switch.screen;
}

static void switch_screen_refresh(void) {
    for (int i = 0; i < SWITCH_SOURCES; i++) {
        bool active = i == (int)np_current_source();
        uint32_t tint = np_source((ui_source_t)i)->tint;
        lv_obj_set_style_bg_opa(s_switch.items[i], active ? UI_OPA_SELECTED : UI_OPA_GLASS, 0);
        lv_obj_set_style_border_color(s_switch.items[i], lv_color_hex(tint), 0);
        lv_obj_set_style_border_opa(s_switch.items[i], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(s_switch.items[i], lv_color_hex(active ? UI_COLOR_TEXT : UI_COLOR_TEXT_DIM), 0);
    }
}

void switch_open(void) {
    switch_screen_refresh();
    nav_show(s_switch.screen);
}

static void switch_back_cb(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_TOP) {
        return;
    }
    nav_show(np_wireless_screen());
}

static void source_pick_cb(lv_event_t *e) {
    ui_source_t source = (ui_source_t)(uintptr_t)lv_event_get_user_data(e);
    if (source != np_current_source()) {
        np_set_current_source(source);
        if (s_source_handler) {
            s_source_handler(source);
        }
    }
    nav_open_pinned(np_wireless_screen());
}

// USB is not offered: it isn't implemented yet.
void switch_build(void) {
    s_switch.screen = ui_screen_create();

    lv_obj_t *title = lv_label_create(s_switch.screen);
    lv_label_set_text(title, "SWITCH SOURCE");
    lv_obj_set_style_text_font(title, UI_FONT_OVERLINE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_letter_space(title, UI_OVERLINE_SPACING, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, UI_TITLE_TOP_Y);

    lv_obj_t *list = lv_obj_create(s_switch.screen);
    lv_obj_remove_style_all(list);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(list, SWITCH_ITEM_W, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, SWITCH_ITEM_GAP, 0);
    lv_obj_align(list, LV_ALIGN_CENTER, 0, SWITCH_LIST_Y);

    for (int i = 0; i < SWITCH_SOURCES; i++) {
        const np_source_info_t *info = np_source((ui_source_t)i);
        lv_obj_t *item = lv_obj_create(list);
        lv_obj_remove_style_all(item);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(item, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(item, UI_SPACE_M, 0);
        lv_obj_set_style_pad_hor(item, UI_SPACE_XL, 0);
        lv_obj_set_style_pad_ver(item, UI_SPACE_L, 0);
        lv_obj_set_style_radius(item, UI_RADIUS_CARD, 0);
        lv_obj_set_style_border_width(item, 1, 0);
        lv_obj_set_style_bg_color(item, lv_color_white(), 0);
        lv_obj_add_event_cb(item, source_pick_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        ui_icon_create(item, info->icon, 20, lv_color_hex(info->tint));
        lv_obj_t *label = lv_label_create(item);
        lv_label_set_text(label, info->label);
        lv_obj_set_style_text_font(label, UI_FONT_TITLE, 0);
        s_switch.items[i] = item;
    }
    switch_screen_refresh();

    ui_home_zone_create(s_switch.screen, switch_back_cb);
}
