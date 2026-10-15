#include <stdio.h>
#include <string.h>

#include "icons.h"
#include "ui_tools.h"

// Bluetooth pairing prompt, over every screen on lv_layer_top() while the
// ESP32 has its pairing window open. The ESP32 runs the state machine;
// this only renders pairing_status_t and reports the user's answers.

#define PAIR_TINT     0x7c6cff
#define PAIR_DIM      0x9a9aa4
#define PAIR_ICON_Y   (-104)
#define PAIR_ICON     28
#define PAIR_TITLE_Y  (-70)
#define PAIR_CODE_Y   (-14)
#define PAIR_BODY_Y   44
#define PAIR_BODY_W   240
#define PAIR_BUTTONS_Y 104

static struct {
    lv_obj_t *scrim;
    lv_obj_t *title;
    lv_obj_t *code;
    lv_obj_t *body;
    lv_obj_t *buttons;
    lv_obj_t *cancel; // labels of the round buttons
    lv_obj_t *accept;
    pairing_status_t status;
    bool available;
    // A result the user tapped away stays hidden until the state moves on.
    uint8_t dismissed_state;
    void (*handler)(ui_pairing_action_t action);
} pr = {.dismissed_state = PAIRING_IDLE};

void ui_set_pairing_handler(void (*handler)(ui_pairing_action_t action)) {
    pr.handler = handler;
}

void ui_pairing_request(ui_pairing_action_t action) {
    if (pr.handler) {
        pr.handler(action);
    }
}

const pairing_status_t *ui_pairing_status(void) {
    return pr.available ? &pr.status : NULL;
}

bool ui_pairing_visible(void) {
    return pr.scrim && !lv_obj_has_flag(pr.scrim, LV_OBJ_FLAG_HIDDEN);
}

static bool answerable(void) {
    return pr.status.state == PAIRING_CONFIRM;
}

static bool result(void) {
    return pr.status.state == PAIRING_DONE || pr.status.state == PAIRING_FAILED;
}

// Cancel in every state that has a button for it; on a result, just hide.
static void cancel(void) {
    if (result()) {
        pr.dismissed_state = pr.status.state;
        ui_set_visible(pr.scrim, false);
        return;
    }
    ui_pairing_request(answerable() ? UI_PAIRING_REJECT : UI_PAIRING_CLOSE);
}

static void cancel_cb(lv_event_t *e) {
    (void)e;
    cancel();
}

static void accept_cb(lv_event_t *e) {
    (void)e;
    if (answerable()) {
        ui_pairing_request(UI_PAIRING_ACCEPT);
    }
}

static void scrim_cb(lv_event_t *e) {
    (void)e;
    if (result()) {
        cancel();
    }
}

static lv_obj_t *label_create(const lv_font_t *font, uint32_t color, int32_t y) {
    lv_obj_t *label = lv_label_create(pr.scrim);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    return label;
}

static void build(void) {
    pr.scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(pr.scrim);
    lv_obj_set_size(pr.scrim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(pr.scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pr.scrim, LV_OPA_COVER, 0);
    lv_obj_add_flag(pr.scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pr.scrim, scrim_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *icon = lv_image_create(pr.scrim);
    lv_image_set_src(icon, &icon_bluetooth);
    lv_image_set_inner_align(icon, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_size(icon, PAIR_ICON, PAIR_ICON);
    lv_obj_set_style_image_recolor(icon, lv_color_hex(PAIR_TINT), 0);
    lv_obj_set_style_image_recolor_opa(icon, LV_OPA_COVER, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, PAIR_ICON_Y);

    pr.title = label_create(&inter_semi_16, 0xffffff, PAIR_TITLE_Y);
    pr.code = label_create(&inter_tnum_52, 0xffffff, PAIR_CODE_Y);
    pr.body = label_create(&inter_med_13, PAIR_DIM, PAIR_BODY_Y);
    lv_obj_set_width(pr.body, PAIR_BODY_W);
    lv_label_set_long_mode(pr.body, LV_LABEL_LONG_MODE_WRAP);

    pr.buttons = tool_button_row_create(pr.scrim);
    lv_obj_align(pr.buttons, LV_ALIGN_CENTER, 0, PAIR_BUTTONS_Y);
    pr.cancel = tool_button_create(pr.buttons, "Cancel", cancel_cb);
    pr.accept = tool_primary_button_create(pr.buttons, lv_color_hex(PAIR_TINT), accept_cb);
    ui_label_set_text(pr.accept, LV_SYMBOL_OK);
}

static void render(void) {
    const pairing_status_t *s = &pr.status;
    char code[8] = "";
    char body[96] = "";
    const char *title = "";

    switch (s->state) {
    case PAIRING_OPEN:
        title = "Pair a device";
        snprintf(body, sizeof(body), "Choose %s in your phone's Bluetooth settings \xC2\xB7 %u:%02u",
                 ui_settings_current()->device_name, s->seconds_left / 60, s->seconds_left % 60);
        break;
    case PAIRING_CONFIRM:
        title = "Confirm the code";
        snprintf(code, sizeof(code), "%06lu", (unsigned long)s->code);
        snprintf(body, sizeof(body), "Pair only if your phone shows the same number");
        break;
    case PAIRING_PASSKEY:
        title = "Type this code";
        snprintf(code, sizeof(code), "%06lu", (unsigned long)s->code);
        snprintf(body, sizeof(body), "on the device you're pairing, then Enter");
        break;
    case PAIRING_BUSY:
        title = "Pairing\xE2\x80\xA6";
        break;
    case PAIRING_DONE:
        title = "Paired";
        snprintf(body, sizeof(body), "%s", s->peer[0] ? s->peer : "New device");
        break;
    case PAIRING_FAILED:
        title = "Didn't pair";
        snprintf(body, sizeof(body), "Try again from your phone");
        break;
    default:
        break;
    }
    ui_label_set_text(pr.title, title);
    ui_label_set_text(pr.code, code);
    ui_label_set_text(pr.body, body);
    ui_set_visible(pr.code, code[0]);
    // Without a code, the message takes its place.
    lv_obj_align(pr.body, LV_ALIGN_CENTER, 0, code[0] ? PAIR_BODY_Y : PAIR_CODE_Y);
    ui_set_visible(pr.buttons, s->state == PAIRING_OPEN || s->state == PAIRING_CONFIRM || s->state == PAIRING_PASSKEY);
    ui_set_visible(lv_obj_get_parent(pr.accept), s->state == PAIRING_CONFIRM);
}

void ui_update_pairing(const pairing_status_t *status) {
    pairing_status_t next = status ? *status : (pairing_status_t){0};
    bool settings_changed = pr.available != (status != NULL) || next.bonded != pr.status.bonded;
    pr.available = status != NULL;
    if (settings_changed) {
        // The settings rows read the count through ui_pairing_status().
        pr.status.bonded = next.bonded;
        ui_settings_refresh();
    }
    if (next.state != pr.dismissed_state) {
        pr.dismissed_state = PAIRING_IDLE;
    }
    bool show = next.state != PAIRING_IDLE && next.state != pr.dismissed_state;
    if (!show) {
        pr.status = next;
        if (pr.scrim) {
            ui_set_visible(pr.scrim, false);
        }
        return;
    }
    if (!pr.scrim) {
        build();
    }
    bool was_visible = ui_pairing_visible();
    if (was_visible && !memcmp(&next, &pr.status, sizeof(next))) {
        return;
    }
    pr.status = next;
    render();
    if (!was_visible) {
        lv_obj_move_foreground(pr.scrim);
        ui_set_visible(pr.scrim, true);
        ui_wake();
    }
}

bool ui_pairing_knob_press(void) {
    if (!ui_pairing_visible()) {
        return false;
    }
    if (answerable()) {
        ui_pairing_request(UI_PAIRING_ACCEPT);
    } else {
        cancel();
    }
    return true;
}

bool ui_pairing_knob_long_press(void) {
    if (!ui_pairing_visible()) {
        return false;
    }
    cancel();
    return true;
}
