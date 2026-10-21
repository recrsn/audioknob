// The WiFi setup screen, home while setup is pending: a join QR and the
// hotspot's details.

#include <stdio.h>
#include <string.h>

#include "ui/assets/fonts.h"
#include "ui/shell/nav.h"
#include "ui/widgets/widgets.h"

// Layout for the round panel: title at the top, QR in the middle, status and
// manual-join details below, all kept inside the circle.
#define SETUP_TITLE_Y   (-132)
#define SETUP_QR_SIZE   168
#define SETUP_QR_Y      (-12)
#define SETUP_STATUS_Y  100
#define SETUP_NETWORK_Y 128

static struct {
    lv_obj_t *screen;
    lv_obj_t *qr;
    lv_obj_t *status_label;
    lv_obj_t *network_label;
    char qr_payload[128];
} wifi_setup_screen;

static lv_obj_t *setup_label_create(lv_obj_t *parent, const lv_font_t *font, lv_opa_t opa, int32_t y) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_opa(label, opa, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, UI_TEXT_W);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    return label;
}

static void wifi_setup_screen_build(void) {
    wifi_setup_screen.screen = ui_screen_create();

    lv_obj_t *title = setup_label_create(wifi_setup_screen.screen, UI_FONT_TITLE, LV_OPA_COVER, SETUP_TITLE_Y);
    lv_label_set_text(title, "Set up WiFi");

    wifi_setup_screen.qr = lv_qrcode_create(wifi_setup_screen.screen);
    lv_qrcode_set_size(wifi_setup_screen.qr, SETUP_QR_SIZE);
    lv_qrcode_set_dark_color(wifi_setup_screen.qr, lv_color_black());
    lv_qrcode_set_light_color(wifi_setup_screen.qr, lv_color_white());
    // Phone cameras need a light margin around the code on a dark screen.
    lv_qrcode_set_quiet_zone(wifi_setup_screen.qr, true);
    lv_obj_align(wifi_setup_screen.qr, LV_ALIGN_CENTER, 0, SETUP_QR_Y);

    wifi_setup_screen.status_label =
        setup_label_create(wifi_setup_screen.screen, UI_FONT_BODY, LV_OPA_COVER, SETUP_STATUS_Y);
    wifi_setup_screen.network_label =
        setup_label_create(wifi_setup_screen.screen, UI_FONT_CAPTION, UI_OPA_TEXT_MUTED, SETUP_NETWORK_Y);
}

void ui_show_wifi_setup(const char *qr_payload, const char *ssid, const char *password, const char *status) {
    if (!wifi_setup_screen.screen) {
        wifi_setup_screen_build();
    }

    // Regenerating the QR is the expensive part; only redo it on change.
    if (strcmp(wifi_setup_screen.qr_payload, qr_payload) != 0) {
        snprintf(wifi_setup_screen.qr_payload, sizeof(wifi_setup_screen.qr_payload), "%s", qr_payload);
        lv_qrcode_update(wifi_setup_screen.qr, qr_payload, strlen(qr_payload));
        lv_label_set_text_fmt(wifi_setup_screen.network_label, "%s  /  %s", ssid, password);
    }
    if (strcmp(lv_label_get_text(wifi_setup_screen.status_label), status) != 0) {
        lv_label_set_text(wifi_setup_screen.status_label, status);
    }

    nav_set_home(wifi_setup_screen.screen);
}
