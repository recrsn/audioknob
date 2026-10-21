#include "ui/shell/audio_chip.h"

#include <string.h>

#include "ui/assets/fonts.h"
#include "ui/assets/icons.h"
#include "ui/home/np_card.h"
#include "ui/now_playing/now_playing.h"
#include "ui/shell/launcher.h"
#include "ui/shell/nav.h"
#include "ui/widgets/widgets.h"

// Whatever of the safe area the icon and padding leave.
#define AUDIO_CHIP_TEXT_W (UI_CHIP_MAX_W - 2 * UI_CHIP_PAD_X - UI_STATUS_ICON_SIZE - UI_SPACE_XS)

static struct {
    lv_obj_t *chip;
    lv_obj_t *icon;     // AirPlay/Bluetooth
    lv_obj_t *amb_icon; // an icons_20 glyph, not an image
#ifdef AK_USB_AUDIO
    lv_obj_t *usb_icon; // LV_SYMBOL_USB has no image asset
#endif
    lv_obj_t *label;
    lv_obj_t *target;   // the source screen a tap goes to
    bool dim;
} audio_chip;

lv_obj_t *audio_chip_target(void) {
    return audio_chip.target;
}

// Only where the user went from the audio, not on the source screens
// themselves or the setup QR.
static bool audio_chip_wanted(void) {
    lv_obj_t *active = lv_screen_active();
    return audio_chip.target && !launcher_visible() && ui_tools_app_of(active) >= 0 && active != audio_chip.target;
}

// A content-sized label wraps rather than dots, so the label is one line
// tall and its width is set per text: the text's own, up to a cap.
static void audio_chip_set_text(const char *text) {
    if (strcmp(lv_label_get_text(audio_chip.label), text) == 0) {
        return;
    }
    lv_point_t size;
    lv_text_get_size(&size, text, lv_obj_get_style_text_font(audio_chip.label, 0), 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    lv_obj_set_width(audio_chip.label, LV_MIN(size.x, AUDIO_CHIP_TEXT_W));
    lv_label_set_text(audio_chip.label, text);
}

void audio_chip_refresh(void) {
    if (!audio_chip.chip) {
        return;
    }
    lv_obj_t *target = np_audio_screen();
    audio_chip.target = target;
    ui_set_visible(audio_chip.chip, audio_chip_wanted());
    np_card_refresh(target);
    if (!target) {
        return;
    }

    const np_audio_t *audio = np_audio();
#ifdef AK_USB_AUDIO
    bool usb = target == np_usb_screen();
    ui_set_visible(audio_chip.usb_icon, usb);
#else
    bool usb = false;
#endif
    bool amb = np_is_ambience(target);
    ui_set_visible(audio_chip.amb_icon, amb);
    ui_set_visible(audio_chip.icon, !usb && !amb);
    if (amb) {
        ui_ambience_t kind;
        ui_ambience_playing(&kind, NULL);
        audio_chip_set_text(ui_ambience_name(kind));
        if (audio_chip.dim) {
            audio_chip.dim = false;
            lv_obj_set_style_text_opa(audio_chip.label, LV_OPA_COVER, 0);
        }
        return;
    }
    if (!usb) {
        const np_source_info_t *source = np_source(audio->wireless_source);
        if (lv_image_get_src(audio_chip.icon) != source->icon) {
            lv_image_set_src(audio_chip.icon, source->icon);
            lv_obj_set_style_image_recolor(audio_chip.icon, lv_color_hex(source->tint), 0);
        }
    }
    audio_chip_set_text(usb ? "USB Audio" : audio->wireless_text);

    // Paused but still connected reads dimmer than playing.
#ifdef AK_USB_AUDIO
    bool dim = usb ? !audio->usb_playing : !audio->wireless_playing;
#else
    bool dim = !audio->wireless_playing;
#endif
    if (dim != audio_chip.dim) {
        audio_chip.dim = dim;
        lv_obj_set_style_text_opa(audio_chip.label, dim ? UI_OPA_TEXT_MUTED : LV_OPA_COVER, 0);
    }
}

void ui_audio_changed(void) {
    audio_chip_refresh();
}

static void audio_chip_cb(lv_event_t *e) {
    (void)e;
    if (audio_chip.target == np_wireless_screen() && np_audio()->wireless_source == UI_SOURCE_RADIO) {
        nav_open_app(UI_APP_RADIO);
    } else {
        nav_open_audio();
    }
}

void audio_chip_build(void) {
    lv_obj_t *chip = ui_status_pill_create(lv_layer_top(), audio_chip_cb);
    ui_chip_align_top(chip);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_HIDDEN);
    audio_chip.chip = chip;

    audio_chip.icon = ui_icon_create(chip, &icon_airplay, UI_STATUS_ICON_SIZE, lv_color_hex(UI_TINT_AIRPLAY));
    audio_chip.amb_icon = lv_label_create(chip);
    lv_label_set_text(audio_chip.amb_icon, ICON_RAIN);
    lv_obj_set_style_text_font(audio_chip.amb_icon, UI_FONT_ICON_SM, 0);
    lv_obj_set_style_text_color(audio_chip.amb_icon, lv_color_hex(UI_TINT_AMBIENCE), 0);
    lv_obj_add_flag(audio_chip.amb_icon, LV_OBJ_FLAG_HIDDEN);
#ifdef AK_USB_AUDIO
    audio_chip.usb_icon = lv_label_create(chip);
    lv_label_set_text(audio_chip.usb_icon, LV_SYMBOL_USB);
    lv_obj_set_style_text_font(audio_chip.usb_icon, UI_FONT_SYMBOL_SM, 0);
    lv_obj_set_style_text_color(audio_chip.usb_icon, lv_color_hex(UI_TINT_USB), 0);
#endif

    audio_chip.label = ui_status_label_create(chip);
    lv_obj_set_style_text_color(audio_chip.label, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_label_set_long_mode(audio_chip.label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(audio_chip.label, lv_font_get_line_height(UI_FONT_BODY));
}
