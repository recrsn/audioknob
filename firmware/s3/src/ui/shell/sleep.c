#include "ui/shell/sleep.h"

#include "ui/internal.h"

static struct {
    lv_obj_t *catcher;
    bool asleep;
    void (*handler)(bool on);
} s_sleep;

void ui_set_display_handler(void (*handler)(bool on)) {
    s_sleep.handler = handler;
}

bool ui_wake(void) {
    lv_display_trigger_activity(NULL);
    if (!s_sleep.asleep) {
        return false;
    }
    s_sleep.asleep = false;
    ui_set_visible(s_sleep.catcher, false);
    if (s_sleep.handler) {
        s_sleep.handler(true);
    }
    return true;
}

static void display_sleep(void) {
    s_sleep.asleep = true;
    ui_set_visible(s_sleep.catcher, true);
    if (s_sleep.handler) {
        s_sleep.handler(false);
    }
}

// Hiding the catcher mid-press would hand the press to whatever is under
// it, so the touch is ignored until the finger lifts.
static void sleep_catcher_cb(lv_event_t *e) {
    (void)e;
    lv_indev_wait_release(lv_indev_active());
    ui_wake();
}

void ui_sleep_build(void) {
    s_sleep.catcher = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(s_sleep.catcher);
    lv_obj_set_size(s_sleep.catcher, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_sleep.catcher, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_sleep.catcher, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_sleep.catcher, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_sleep.catcher, sleep_catcher_cb, LV_EVENT_PRESSED, NULL);
}

// Something that must be seen (a ringing countdown, the pairing or update
// prompt) keeps the display on.
void ui_sleep_tick(void) {
    uint32_t sleep_s = ui_settings_current()->sleep_s;
    if (s_sleep.asleep || !sleep_s || ui_tools_alerting() || ui_pairing_visible() ||
        ui_firmware_visible()) {
        return;
    }
    if (lv_display_get_inactive_time(NULL) >= sleep_s * 1000) {
        display_sleep();
    }
}
