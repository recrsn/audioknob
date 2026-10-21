#include "ui/home/band.h"

#include "ui/assets/icons.h"
#include "ui/shell/nav.h"
#include "ui/widgets/widgets.h"

static struct {
    lv_obj_t *band;
    lv_obj_t *glyph;
    lv_obj_t *label;
    int app;
} s_band;

bool band_app_item(ui_app_t app, band_item_t *out) {
    if (!ui_tools_status(app, out->text, sizeof(out->text))) {
        return false;
    }
    ui_app_info_t info = ui_tools_info(app);
    out->app = app;
    out->icon = info.icon;
    out->tint = info.tint;
    return true;
}

bool band_item(band_item_t *out) {
    return band_app_item(UI_APP_TALK, out) || band_app_item(UI_APP_INBOX, out);
}

bool band_owns(ui_app_t app) {
    return app == UI_APP_TALK || app == UI_APP_INBOX;
}

static void band_cb(lv_event_t *e) {
    (void)e;
    if (s_band.app >= 0) {
        nav_open_app((ui_app_t)s_band.app);
    }
}

void band_build(lv_obj_t *screen) {
    s_band.label = ui_glyph_pill_create(screen, ICON_INBOX, lv_color_hex(UI_TINT_TALK), band_cb, NULL);
    s_band.band = lv_obj_get_parent(s_band.label);
    s_band.glyph = lv_obj_get_child(s_band.band, 0);
    ui_chip_align_top(s_band.band);
    lv_obj_add_flag(s_band.band, LV_OBJ_FLAG_HIDDEN);
}

void band_refresh(void) {
    band_item_t item;
    bool show = band_item(&item);
    ui_set_visible(s_band.band, show);
    if (!show) {
        return;
    }
    s_band.app = item.app;
    ui_label_set_text(s_band.glyph, item.icon);
    lv_obj_set_style_text_color(s_band.glyph, item.tint, 0);
    ui_label_set_text(s_band.label, item.text);
}
