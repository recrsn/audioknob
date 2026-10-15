#include <stdio.h>
#include <string.h>

#include "icons.h"
#include "ui_prompt.h"
#include "ui_tools.h"

// Bluetooth pairing prompt, over every screen on lv_layer_top() while the
// ESP32 has its pairing window open. The ESP32 runs the state machine;
// this only renders pairing_status_t and reports the user's answers.

#define PAIR_TINT 0x7c6cff

static struct {
    ui_prompt_t prompt;
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
    return ui_prompt_visible(&pr.prompt);
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
        ui_prompt_show(&pr.prompt, false);
        return;
    }
    ui_pairing_request(answerable() ? UI_PAIRING_REJECT : UI_PAIRING_CLOSE);
}

static void accept(void) {
    if (answerable()) {
        ui_pairing_request(UI_PAIRING_ACCEPT);
    }
}

// A tap anywhere dismisses a result.
static void scrim_tap(void) {
    if (result()) {
        cancel();
    }
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
    bool waiting = s->state == PAIRING_OPEN || s->state == PAIRING_CONFIRM || s->state == PAIRING_PASSKEY;
    ui_prompt_render(&pr.prompt, &(ui_prompt_content_t){.image = &icon_bluetooth,
                                                         .tint = PAIR_TINT,
                                                         .title = title,
                                                         .code = code,
                                                         .body = body,
                                                         .cancel = waiting,
                                                         .accept = s->state == PAIRING_CONFIRM});
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
        if (pr.prompt.scrim) {
            ui_prompt_show(&pr.prompt, false);
        }
        return;
    }
    if (!pr.prompt.scrim) {
        ui_prompt_build(&pr.prompt, cancel, accept, scrim_tap);
    }
    bool was_visible = ui_pairing_visible();
    if (was_visible && !memcmp(&next, &pr.status, sizeof(next))) {
        return;
    }
    pr.status = next;
    render();
    if (!was_visible) {
        ui_prompt_show(&pr.prompt, true);
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
