#include "ui/widgets/prompt.h"

#include "ui/apps/tools.h"

#define PROMPT_ICON_Y    (-104)
#define PROMPT_ICON      28
#define PROMPT_TITLE_Y   (-70)
#define PROMPT_CODE_Y    (-14)
#define PROMPT_BODY_Y    44
#define PROMPT_BUTTONS_Y 104

static void scrim_cb(lv_event_t *e) {
    void (*fn)(void) = (void (*)(void))lv_event_get_user_data(e);
    if (fn) {
        fn();
    }
}

// The tool buttons take no user data, so each carries its action on the
// object itself.
static void button_cb(lv_event_t *e) {
    void (*fn)(void) = (void (*)(void))lv_obj_get_user_data(lv_event_get_current_target(e));
    if (fn) {
        fn();
    }
}

static lv_obj_t *label_create(lv_obj_t *parent, const lv_font_t *font, uint32_t color, int32_t y) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, y);
    return label;
}

void ui_prompt_build(ui_prompt_t *p, void (*on_cancel)(void), void (*on_accept)(void), void (*on_scrim)(void)) {
    p->scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(p->scrim);
    lv_obj_set_size(p->scrim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(p->scrim, lv_color_hex(UI_COLOR_BG), 0);
    lv_obj_set_style_bg_opa(p->scrim, LV_OPA_COVER, 0);
    lv_obj_add_flag(p->scrim, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(p->scrim, scrim_cb, LV_EVENT_CLICKED, (void *)on_scrim);

    p->image = lv_image_create(p->scrim);
    lv_image_set_inner_align(p->image, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_size(p->image, PROMPT_ICON, PROMPT_ICON);
    lv_obj_set_style_image_recolor_opa(p->image, LV_OPA_COVER, 0);
    lv_obj_align(p->image, LV_ALIGN_CENTER, 0, PROMPT_ICON_Y);
    p->symbol = label_create(p->scrim, UI_FONT_SYMBOL_LG, UI_COLOR_TEXT, PROMPT_ICON_Y);

    p->title = label_create(p->scrim, UI_FONT_TITLE, UI_COLOR_TEXT, PROMPT_TITLE_Y);
    p->code = label_create(p->scrim, UI_FONT_READING, UI_COLOR_TEXT, PROMPT_CODE_Y);
    p->body = label_create(p->scrim, UI_FONT_BODY, UI_COLOR_TEXT_DIM, PROMPT_BODY_Y);
    lv_obj_set_width(p->body, UI_TEXT_W);
    lv_label_set_long_mode(p->body, LV_LABEL_LONG_MODE_WRAP);

    p->buttons = tool_button_row_create(p->scrim);
    lv_obj_align(p->buttons, LV_ALIGN_CENTER, 0, PROMPT_BUTTONS_Y);
    p->cancel = lv_obj_get_parent(tool_button_create(p->buttons, "Cancel", button_cb));
    lv_obj_set_user_data(p->cancel, (void *)on_cancel);
    lv_obj_t *accept = tool_primary_button_create(p->buttons, lv_color_hex(UI_COLOR_TEXT), button_cb);
    ui_label_set_text(accept, LV_SYMBOL_OK);
    p->accept = lv_obj_get_parent(accept);
    lv_obj_set_user_data(p->accept, (void *)on_accept);
}

void ui_prompt_render(ui_prompt_t *p, const ui_prompt_content_t *c) {
    lv_color_t tint = lv_color_hex(c->tint);
    ui_set_visible(p->image, c->image != NULL);
    ui_set_visible(p->symbol, c->image == NULL && c->symbol != NULL);
    if (c->image) {
        if (lv_image_get_src(p->image) != c->image) {
            lv_image_set_src(p->image, c->image);
        }
        lv_obj_set_style_image_recolor(p->image, tint, 0);
    } else if (c->symbol) {
        ui_label_set_text(p->symbol, c->symbol);
        lv_obj_set_style_text_color(p->symbol, tint, 0);
    }

    const char *code = c->code ? c->code : "";
    ui_label_set_text(p->title, c->title ? c->title : "");
    ui_label_set_text(p->code, code);
    ui_label_set_text(p->body, c->body ? c->body : "");
    ui_set_visible(p->code, code[0]);
    // Without a code, the message takes its place.
    lv_obj_align(p->body, LV_ALIGN_CENTER, 0, code[0] ? PROMPT_BODY_Y : PROMPT_CODE_Y);

    ui_set_visible(p->buttons, c->cancel || c->accept);
    ui_set_visible(p->cancel, c->cancel);
    ui_set_visible(p->accept, c->accept);
    lv_obj_set_style_bg_color(p->accept, tint, 0);
}

bool ui_prompt_visible(const ui_prompt_t *p) {
    return p->scrim && !lv_obj_has_flag(p->scrim, LV_OBJ_FLAG_HIDDEN);
}

void ui_prompt_show(ui_prompt_t *p, bool show) {
    if (show && !ui_prompt_visible(p)) {
        lv_obj_move_foreground(p->scrim);
    }
    ui_set_visible(p->scrim, show);
}
