#include <stdio.h>
#include <string.h>

#include "ui_prompt.h"
#include "ui_tools.h"

// Firmware updates from the web page, on the same prompt as pairing:
// progress while an image arrives and while it's sent to the Bluetooth chip,
// "Install update?" in between, then the result. The upload only writes the
// spare slot; nothing boots it until someone at the device says yes, so the
// unauthenticated page can't replace the firmware by itself.

// Between the percent and the message, which moves down to make room.
#define FW_BAR_Y     20
#define FW_BAR_W     180
#define FW_BAR_H     4
#define FW_DETAIL_Y  52
#define FW_RESULT_MS 5000

static struct {
    ui_prompt_t prompt;
    lv_obj_t *bar;
    lv_timer_t *result_timer;
    ui_firmware_state_t state; // the last one pushed
    unsigned progress;
    bool bluetooth;
    char version[32];
    void (*handler)(bool install);
} fw;

void ui_set_firmware_handler(void (*handler)(bool install)) {
    fw.handler = handler;
}

bool ui_firmware_visible(void) {
    return ui_prompt_visible(&fw.prompt);
}

static void hide(void) {
    if (fw.result_timer) {
        lv_timer_delete(fw.result_timer);
        fw.result_timer = NULL;
    }
    if (fw.prompt.scrim) {
        ui_prompt_show(&fw.prompt, false);
    }
}

static bool showing_result(void) {
    return ui_firmware_visible() && (fw.state == UI_FIRMWARE_INSTALLED || fw.state == UI_FIRMWARE_FAILED ||
                                     fw.state == UI_FIRMWARE_CURRENT || fw.state == UI_FIRMWARE_CHECK_FAILED);
}

static void answer(bool install) {
    hide();
    if (fw.handler) {
        fw.handler(install);
    }
}

static void decline(void) {
    answer(false);
}

static void install(void) {
    answer(true);
}

static void scrim_tapped(void) {
    if (showing_result()) {
        hide();
    }
}

static void result_timer_cb(lv_timer_t *t) {
    (void)t;
    fw.result_timer = NULL; // a one-shot deletes itself
    hide();
}

static void build(void) {
    ui_prompt_build(&fw.prompt, decline, install, scrim_tapped);
    // Only the bar's own 180x4 box is masked, not the screen.
    fw.bar = lv_bar_create(fw.prompt.scrim);
    lv_obj_remove_style_all(fw.bar);
    lv_obj_remove_flag(fw.bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(fw.bar, FW_BAR_W, FW_BAR_H);
    lv_obj_align(fw.bar, LV_ALIGN_CENTER, 0, FW_BAR_Y);
    lv_bar_set_range(fw.bar, 0, 100);
    lv_obj_set_style_radius(fw.bar, UI_RADIUS_HAIRLINE, 0);
    lv_obj_set_style_radius(fw.bar, UI_RADIUS_HAIRLINE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(fw.bar, lv_color_hex(UI_COLOR_CONTROL), 0);
    lv_obj_set_style_bg_opa(fw.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(fw.bar, LV_OPA_COVER, LV_PART_INDICATOR);
}

static void show(void) {
    ui_prompt_show(&fw.prompt, true);
    ui_wake();
}

static void render_ready(const ui_firmware_t *f) {
    char body[96];
    snprintf(body, sizeof(body), "Version %s is ready. %s", f->version,
             f->bluetooth ? "Bluetooth restarts when it's in." : "The device restarts.");
    ui_prompt_render(&fw.prompt, &(ui_prompt_content_t){.symbol = LV_SYMBOL_DOWNLOAD,
                                                         .tint = UI_COLOR_ACCENT,
                                                         .title = f->bluetooth ? "Update Bluetooth?" : "Install update?",
                                                         .body = body,
                                                         .cancel = true,
                                                         .accept = true});
    ui_set_visible(fw.bar, false);
}

static void render_progress(const ui_firmware_t *f) {
    bool sending = f->state == UI_FIRMWARE_INSTALLING;
    uint32_t tint = sending ? UI_TINT_BLUETOOTH : UI_COLOR_ACCENT;
    unsigned percent = f->progress > 100 ? 100 : f->progress;
    char code[8];
    char body[64] = "";
    snprintf(code, sizeof(code), "%u%%", percent);
    if (sending && percent == 100) {
        snprintf(body, sizeof(body), "Waiting for Bluetooth to restart");
    } else if (f->version[0]) {
        snprintf(body, sizeof(body), "%s %s", f->bluetooth ? "Bluetooth firmware" : "Version", f->version);
    }
    ui_prompt_render(&fw.prompt, &(ui_prompt_content_t){.symbol = sending ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_DOWNLOAD,
                                                         .tint = tint,
                                                         .title = sending ? "Updating Bluetooth" : "Receiving update",
                                                         .code = code,
                                                         .body = body});
    lv_obj_align(fw.prompt.body, LV_ALIGN_CENTER, 0, FW_DETAIL_Y);
    lv_obj_set_style_bg_color(fw.bar, lv_color_hex(tint), LV_PART_INDICATOR);
    lv_bar_set_value(fw.bar, (int32_t)percent, LV_ANIM_OFF);
    ui_set_visible(fw.bar, true);
}

static void render_result(const ui_firmware_t *f) {
    bool ok = f->state == UI_FIRMWARE_INSTALLED;
    char body[64];
    if (ok) {
        snprintf(body, sizeof(body), "Updated to %s.", f->version);
    } else {
        snprintf(body, sizeof(body), "%s", f->error[0] ? f->error : "Something went wrong.");
    }
    ui_prompt_render(&fw.prompt, &(ui_prompt_content_t){
                                     .symbol = ok ? LV_SYMBOL_OK : LV_SYMBOL_WARNING,
                                     .tint = ok ? UI_COLOR_ACCENT : UI_COLOR_DANGER,
                                     .title = ok ? (f->bluetooth ? "Bluetooth updated" : "Update installed")
                                                 : "Update failed",
                                     .body = body});
    ui_set_visible(fw.bar, false);
}

void ui_update_firmware(const ui_firmware_t *f) {
    bool changed = f->state != fw.state || f->bluetooth != fw.bluetooth ||
                   strcmp(fw.version, f->version);
    bool result = f->state == UI_FIRMWARE_INSTALLED || f->state == UI_FIRMWARE_FAILED ||
                  f->state == UI_FIRMWARE_CURRENT || f->state == UI_FIRMWARE_CHECK_FAILED;
    // A result shows once, on the change; progress redraws as it moves.
    if (!changed && (result || f->progress == fw.progress)) {
        return;
    }
    fw.state = f->state;
    fw.bluetooth = f->bluetooth;
    snprintf(fw.version, sizeof(fw.version), "%s", f->version);
    fw.progress = f->progress;
    if (f->state == UI_FIRMWARE_IDLE) {
        hide();
        return;
    }
    if (!fw.prompt.scrim) {
        build();
    }
    if (fw.result_timer) {
        lv_timer_delete(fw.result_timer);
        fw.result_timer = NULL;
    }
    switch (f->state) {
    case UI_FIRMWARE_AVAILABLE: {
        char body[128];
        snprintf(body, sizeof(body), "%s %s is available. Download now? You'll confirm again before installing.",
                 f->bluetooth ? "Bluetooth" : "Version", f->version);
        ui_prompt_render(&fw.prompt, &(ui_prompt_content_t){.symbol = LV_SYMBOL_DOWNLOAD,
            .tint = UI_COLOR_ACCENT, .title = "Update available", .body = body, .cancel = true, .accept = true});
        ui_set_visible(fw.bar, false);
        break;
    }
    case UI_FIRMWARE_CHECKING:
    case UI_FIRMWARE_CURRENT:
    case UI_FIRMWARE_CHECK_FAILED:
        ui_prompt_render(&fw.prompt, &(ui_prompt_content_t){.symbol = LV_SYMBOL_DOWNLOAD,
            .tint = UI_COLOR_ACCENT,
            .title = f->state == UI_FIRMWARE_CHECKING ? "Checking for updates" :
                     f->state == UI_FIRMWARE_CURRENT ? "You're up to date" : "Couldn't check updates",
            .body = f->state == UI_FIRMWARE_CHECK_FAILED ? f->error : "Checks automatically every day."});
        ui_set_visible(fw.bar, false);
        if (result) {
            fw.result_timer = lv_timer_create(result_timer_cb, FW_RESULT_MS, NULL);
            lv_timer_set_repeat_count(fw.result_timer, 1);
        }
        break;
    case UI_FIRMWARE_READY:
        render_ready(f);
        break;
    case UI_FIRMWARE_RECEIVING:
    case UI_FIRMWARE_INSTALLING:
        render_progress(f);
        break;
    case UI_FIRMWARE_INSTALLED:
    case UI_FIRMWARE_FAILED:
        render_result(f);
        fw.result_timer = lv_timer_create(result_timer_cb, FW_RESULT_MS, NULL);
        lv_timer_set_repeat_count(fw.result_timer, 1);
        break;
    default:
        return;
    }
    if (changed) {
        show();
    }
}

// The knob answers the prompt and dismisses a result; while progress is up
// it does nothing else.
bool ui_firmware_knob_press(void) {
    if (!ui_firmware_visible()) {
        return false;
    }
    if (fw.state == UI_FIRMWARE_READY || fw.state == UI_FIRMWARE_AVAILABLE) {
        install();
    } else if (showing_result()) {
        hide();
    }
    return true;
}

bool ui_firmware_knob_long_press(void) {
    if (!ui_firmware_visible()) {
        return false;
    }
    if (fw.state == UI_FIRMWARE_READY || fw.state == UI_FIRMWARE_AVAILABLE) {
        decline();
    } else if (showing_result()) {
        hide();
    }
    return true;
}
