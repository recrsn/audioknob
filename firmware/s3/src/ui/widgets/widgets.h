#pragma once

// Building blocks the screens share: chips, pills, labels and the home
// swipe zone. ui_round_button_create(), ui_label_set_text() and
// ui_set_visible() are declared in ui/internal.h, for the app pages too.

#include "ui/internal.h"

#define UI_STATUS_ICON_SIZE 14

// LVGL only looks for a child under a touch inside its parent's bounds, so
// a padded hit area sticking out of a content-sized container is cut off.
// Padding the container as much as its children's hit areas reach puts
// them back; the containers are centered, so nothing moves.
void ui_touch_pad(lv_obj_t *container, int32_t ext);

lv_obj_t *ui_screen_create(void);
// mock-react's .home-swipe-zone: a thin pill at the south compass point is
// the only affordance, the whole transparent zone is the hit target. Swipe
// up (or tap) runs cb.
void ui_home_zone_create(lv_obj_t *parent, lv_event_cb_t cb);
// Icon(s) + text in a row, laid out with flex so LVGL computes the internal
// spacing, but positioned via a caller-supplied Y since the surrounding
// screens don't all agree on where that Y should be.
lv_obj_t *ui_flex_row_create(lv_obj_t *parent, int32_t pad_column, int32_t y);
lv_obj_t *ui_icon_create(lv_obj_t *parent, const lv_image_dsc_t *src, int32_t size, lv_color_t color);
// Every chip: a translucent pill of icon + text, one height, capped to the
// safe area's width, tappable past its edges when given a callback.
lv_obj_t *ui_chip_create(lv_obj_t *parent, int32_t pad_column, lv_event_cb_t cb, void *user_data);
// In the top chip band.
void ui_chip_align_top(lv_obj_t *chip);
lv_obj_t *ui_status_pill_create(lv_obj_t *parent, lv_event_cb_t cb);
lv_obj_t *ui_status_label_create(lv_obj_t *row);
// A chip of an icons_20 glyph plus a label, which is returned.
lv_obj_t *ui_glyph_pill_create(lv_obj_t *parent, const char *icon, lv_color_t tint, lv_event_cb_t cb,
                               void *user_data);
lv_obj_t *ui_marquee_label_create(lv_obj_t *parent, const lv_font_t *font, lv_color_t color);
lv_obj_t *ui_glance_label_create(lv_obj_t *parent, const lv_font_t *font, lv_opa_t opa);
