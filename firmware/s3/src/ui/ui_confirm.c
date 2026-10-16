#include "ui_prompt.h"
#include "ui_tools.h"

// "Are you sure?" for anything that can't be undone from the device, on
// the same full-screen prompt as pairing.

static struct {
    ui_prompt_t prompt;
    void (*on_confirm)(void);
} cf;

static void close_prompt(void) {
    cf.on_confirm = NULL;
    ui_prompt_show(&cf.prompt, false);
}

static void accept(void) {
    void (*on_confirm)(void) = cf.on_confirm;
    close_prompt();
    if (on_confirm) {
        on_confirm();
    }
}

void ui_confirm(const char *symbol, const char *title, const char *body, void (*on_confirm)(void)) {
    if (!cf.prompt.scrim) {
        ui_prompt_build(&cf.prompt, close_prompt, accept, NULL);
    }
    cf.on_confirm = on_confirm;
    ui_prompt_render(&cf.prompt, &(ui_prompt_content_t){.symbol = symbol,
                                                         .tint = UI_COLOR_DANGER,
                                                         .title = title,
                                                         .body = body,
                                                         .cancel = true,
                                                         .accept = true});
    ui_prompt_show(&cf.prompt, true);
}

bool ui_confirm_visible(void) {
    return ui_prompt_visible(&cf.prompt);
}

bool ui_confirm_knob_press(void) {
    if (!ui_confirm_visible()) {
        return false;
    }
    accept();
    return true;
}

bool ui_confirm_knob_long_press(void) {
    if (!ui_confirm_visible()) {
        return false;
    }
    close_prompt();
    return true;
}
