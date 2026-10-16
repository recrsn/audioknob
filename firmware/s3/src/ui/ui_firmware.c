#include <stdio.h>
#include <string.h>

#include "ui_prompt.h"
#include "ui_tools.h"

// "Install update?" for firmware uploaded from the web page, on the same
// prompt as pairing. The upload only writes the spare slot; nothing boots it
// until someone at the device says yes, so the unauthenticated page can't
// replace the firmware by itself.

static struct {
    ui_prompt_t prompt;
    void (*handler)(bool install);
} fw;

void ui_set_firmware_handler(void (*handler)(bool install)) {
    fw.handler = handler;
}

bool ui_firmware_visible(void) {
    return ui_prompt_visible(&fw.prompt);
}

static void answer(bool install) {
    ui_prompt_show(&fw.prompt, false);
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

void ui_update_firmware(const char *pending_version, bool bluetooth) {
    if (!pending_version) {
        if (fw.prompt.scrim) {
            ui_prompt_show(&fw.prompt, false);
        }
        return;
    }
    if (ui_firmware_visible()) {
        return;
    }
    if (!fw.prompt.scrim) {
        ui_prompt_build(&fw.prompt, decline, install, NULL);
    }
    char body[96];
    snprintf(body, sizeof(body), "Version %s, sent from the web page. %s", pending_version,
             bluetooth ? "Bluetooth restarts when it's in." : "The device restarts.");
    ui_prompt_render(&fw.prompt, &(ui_prompt_content_t){.symbol = LV_SYMBOL_DOWNLOAD,
                                                         .tint = UI_COLOR_ACCENT,
                                                         .title = bluetooth ? "Update Bluetooth?" : "Install update?",
                                                         .body = body,
                                                         .cancel = true,
                                                         .accept = true});
    ui_prompt_show(&fw.prompt, true);
    ui_wake();
}

bool ui_firmware_knob_press(void) {
    if (!ui_firmware_visible()) {
        return false;
    }
    install();
    return true;
}

bool ui_firmware_knob_long_press(void) {
    if (!ui_firmware_visible()) {
        return false;
    }
    decline();
    return true;
}
