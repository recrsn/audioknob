#include "ui/widgets/widgets.h"

#include <string.h>

#include "ui/assets/fonts.h"

#define HOME_ZONE_W 160
#define HOME_ZONE_H 52

void ui_touch_pad(lv_obj_t *container, int32_t ext) {
    lv_obj_set_style_pad_all(container, ext, 0);
}

lv_obj_t *ui_screen_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

void ui_home_zone_create(lv_obj_t *parent, lv_event_cb_t cb) {
    lv_obj_t *zone = lv_obj_create(parent);
    lv_obj_remove_style_all(zone);
    lv_obj_set_size(zone, HOME_ZONE_W, HOME_ZONE_H);
    lv_obj_align(zone, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_remove_flag(zone, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zone, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(zone, cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t *pill = lv_obj_create(zone);
    lv_obj_remove_style_all(pill);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(pill, 46, 4);
    lv_obj_set_style_radius(pill, UI_RADIUS_HAIRLINE, 0);
    lv_obj_set_style_bg_color(pill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_60, 0);
    // Pages may draw under it (the tool dials), so it carries its own
    // contrast instead of relying on a dark backdrop.
    lv_obj_set_style_outline_width(pill, 2, 0);
    lv_obj_set_style_outline_color(pill, lv_color_black(), 0);
    lv_obj_set_style_outline_opa(pill, LV_OPA_50, 0);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -14);
}

lv_obj_t *ui_flex_row_create(lv_obj_t *parent, int32_t pad_column, int32_t y) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, pad_column, 0);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, y);
    return row;
}

// lv_image_set_scale() only scales the drawn pixels, not the layout box, so
// a scaled-down icon would still reserve its full 64px in a flex row.
// Stretching into an explicitly sized box shrinks both.
lv_obj_t *ui_icon_create(lv_obj_t *parent, const lv_image_dsc_t *src, int32_t size, lv_color_t color) {
    lv_obj_t *icon = lv_image_create(parent);
    lv_image_set_src(icon, src);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_size(icon, size, size);
    lv_obj_set_style_image_recolor(icon, color, 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    return icon;
}

lv_obj_t *ui_chip_create(lv_obj_t *parent, int32_t pad_column, lv_event_cb_t cb, void *user_data) {
    lv_obj_t *chip = ui_flex_row_create(parent, pad_column, 0);
    lv_obj_set_height(chip, UI_CHIP_H);
    lv_obj_set_style_max_width(chip, UI_CHIP_MAX_W, 0);
    lv_obj_set_style_pad_hor(chip, UI_CHIP_PAD_X, 0);
    lv_obj_set_style_radius(chip, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(chip, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(chip, UI_OPA_GLASS, 0);
    if (cb) {
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(chip, UI_TOUCH_EXT);
        lv_obj_add_event_cb(chip, cb, LV_EVENT_CLICKED, user_data);
    }
    return chip;
}

void ui_chip_align_top(lv_obj_t *chip) {
    lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, UI_CHIP_INSET);
}

// mock-react's .status-row.tappable: the pill is the only hint that it's
// the source indicator.
lv_obj_t *ui_status_pill_create(lv_obj_t *parent, lv_event_cb_t cb) {
    return ui_chip_create(parent, UI_SPACE_XS, cb, NULL);
}

lv_obj_t *ui_status_label_create(lv_obj_t *row) {
    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    lv_obj_set_style_text_font(label, UI_FONT_BODY, 0);
    return label;
}

lv_obj_t *ui_glyph_pill_create(lv_obj_t *parent, const char *icon, lv_color_t tint, lv_event_cb_t cb,
                               void *user_data) {
    lv_obj_t *pill = ui_chip_create(parent, UI_SPACE_XS, cb, user_data);

    lv_obj_t *glyph = lv_label_create(pill);
    lv_label_set_text(glyph, icon);
    lv_obj_set_style_text_font(glyph, UI_FONT_ICON_SM, 0);
    lv_obj_set_style_text_color(glyph, tint, 0);

    lv_obj_t *label = lv_label_create(pill);
    lv_obj_set_style_text_font(label, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    return label;
}

lv_obj_t *ui_marquee_label_create(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_width(label, UI_TEXT_W);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

// One now-playing transport button (mock-react's .now-btn / .now-btn.primary):
// a circular chip with a centered glyph. Returns the inner label so callers
// can swap its symbol/color later (e.g. play <-> pause). `dim` renders it as
// mock-react's `.now-btn.disabled` -- there's no reverse UART channel yet for
// touch-originated transport commands (no MSG_TRANSPORT_CMD in protocol.h),
// so prev/next are shown for layout fidelity but stay visually inert rather
// than pretending to work.
lv_obj_t *ui_round_button_create(lv_obj_t *parent, const char *symbol, int32_t size,
                                 const lv_font_t *font, bool dim) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(btn, size, size);
    lv_obj_set_style_radius(btn, UI_RADIUS_PILL, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_COLOR_SURFACE), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    if (dim) {
        lv_obj_set_style_opa(btn, UI_OPA_DISABLED, 0);
    }

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_center(label);
    return label;
}

// The state poll pushes every 33 ms; lv_label_set_text() frees and reallocates
// the string and redraws even when it's unchanged, churning LVGL's heap.
void ui_label_set_text(lv_obj_t *label, const char *text) {
    if (strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

void ui_set_visible(lv_obj_t *obj, bool visible) {
    if (visible) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

lv_obj_t *ui_glance_label_create(lv_obj_t *parent, const lv_font_t *font, lv_opa_t opa) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, UI_TEXT_W);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    return label;
}
