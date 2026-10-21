#include "ui/home/np_card.h"

#include "ui/assets/fonts.h"
#include "ui/assets/icons.h"
#include "ui/home/home.h"
#include "ui/now_playing/now_playing.h"
#include "ui/shell/audio_chip.h"
#include "ui/shell/nav.h"
#include "ui/widgets/widgets.h"

#define NP_CARD_W     212
#define NP_IDLE_GLYPH   UI_COLOR_ACCENT
#define NP_IDLE_GLYPH_W 18
#define NP_CARD_H     48
#define NP_CARD_PAD   UI_SPACE_S
#define NP_THUMB      UI_BUTTON_SIZE_XS

// What's playing under the clock, a paused track to resume, or that the
// device is ready to play: the clock's way back to the audio.
static struct {
    lv_obj_t *card;
    lv_obj_t *thumb;
    lv_obj_t *icon;
    lv_obj_t *idle_icon;
    lv_obj_t *amb_icon;
    lv_obj_t *art;
    lv_obj_t *text;
    lv_obj_t *title;
    lv_obj_t *subtitle;
    lv_obj_t *play;
    int8_t quiet; // the idle styling applied: 1, 0, or -1 before either
    uint32_t art_version;
    lv_color_t tint;
    lv_color_t icon_color;
} np;

static void np_card_cb(lv_event_t *e) {
    (void)e;
    nav_open_audio();
}

// Ambience only stops; a stream pauses.
static void np_play_cb(lv_event_t *e) {
    (void)e;
    if (np_is_ambience(audio_chip_target())) {
        ui_ambience_stop();
    } else {
        np_transport(UI_TRANSPORT_PLAY_PAUSE);
    }
}

void np_card_build(lv_obj_t *col) {
    lv_obj_t *card = lv_obj_create(col);
    lv_obj_remove_style_all(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, np_card_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_size(card, NP_CARD_W, NP_CARD_H);
    lv_obj_set_style_margin_top(card, HOME_COMP_GAP, 0);
    lv_obj_set_style_pad_all(card, NP_CARD_PAD, 0);
    lv_obj_set_style_pad_column(card, UI_SPACE_S, 0);
    lv_obj_set_style_radius(card, UI_RADIUS_PILL, 0);
    // A bigger chip, not a panel: the clock stays the face.
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, UI_OPA_GLASS, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    np.card = card;
    np.quiet = -1;

    // Cover art when the source sends some, else the source's icon.
    lv_obj_t *thumb = lv_obj_create(card);
    lv_obj_remove_style_all(thumb);
    lv_obj_remove_flag(thumb, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(thumb, NP_THUMB, NP_THUMB);
    lv_obj_set_style_radius(thumb, UI_RADIUS_PILL, 0);
    lv_obj_set_style_clip_corner(thumb, true, 0);
    lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
    np.thumb = thumb;
    np.icon = ui_icon_create(thumb, &icon_airplay, 16, lv_color_hex(UI_COLOR_TEXT));
    lv_obj_center(np.icon);
    np.idle_icon = lv_label_create(thumb);
    lv_label_set_text(np.idle_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(np.idle_icon, UI_FONT_SYMBOL, 0);
    lv_obj_set_style_text_color(np.idle_icon, lv_color_hex(NP_IDLE_GLYPH), 0);
    lv_obj_center(np.idle_icon);
    np.amb_icon = lv_label_create(thumb);
    lv_label_set_text(np.amb_icon, ICON_RAIN);
    lv_obj_set_style_text_font(np.amb_icon, UI_FONT_ICON_SM, 0);
    lv_obj_set_style_text_color(np.amb_icon, lv_color_white(), 0);
    lv_obj_center(np.amb_icon);
    lv_obj_add_flag(np.amb_icon, LV_OBJ_FLAG_HIDDEN);
    np.art = lv_image_create(thumb);
    lv_image_set_inner_align(np.art, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_set_size(np.art, NP_THUMB, NP_THUMB);
    lv_obj_add_flag(np.art, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *text = lv_obj_create(card);
    np.text = text;
    lv_obj_remove_style_all(text);
    lv_obj_remove_flag(text, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(text, UI_SPACE_XXS, 0);
    np.title = lv_label_create(text);
    np.subtitle = lv_label_create(text);
    lv_obj_t *labels[] = {np.title, np.subtitle};
    for (int i = 0; i < 2; i++) {
        lv_obj_set_width(labels[i], lv_pct(100));
        lv_label_set_long_mode(labels[i], LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(labels[i], "");
    }
    lv_obj_set_style_text_font(np.title, UI_FONT_LABEL, 0);
    lv_obj_set_style_text_color(np.title, lv_color_hex(UI_COLOR_TEXT), 0);
    lv_obj_set_style_text_font(np.subtitle, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(np.subtitle, lv_color_hex(UI_COLOR_TEXT_DIM), 0);
    // A content-tall label wraps rather than dots.
    for (int i = 0; i < 2; i++) {
        lv_obj_set_height(labels[i], lv_font_get_line_height(lv_obj_get_style_text_font(labels[i], 0)));
    }

    np.play = ui_round_button_create(card, LV_SYMBOL_PLAY, UI_BUTTON_SIZE_XS, UI_FONT_SYMBOL_SM, false);
    lv_obj_t *btn = lv_obj_get_parent(np.play);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    // Out to the card's edge, which is as far as LVGL looks for it; the
    // rest of the card opens the source.
    lv_obj_set_ext_click_area(btn, NP_CARD_PAD);
    lv_obj_add_event_cb(btn, np_play_cb, LV_EVENT_CLICKED, NULL);
}

// Not playing, the row is a plain line like the weather's: a glyph and its
// text, centered, with no pill, tile or button. Playing, it's the pill with
// art and play/pause. Applied only on a change:
// the row refreshes on every state push.
static void np_card_set_quiet(bool quiet) {
    if (np.quiet == quiet) {
        return;
    }
    np.quiet = quiet;
    lv_obj_set_style_bg_opa(np.card, quiet ? LV_OPA_TRANSP : UI_OPA_GLASS, 0);
    lv_obj_set_flex_align(np.card, quiet ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(np.thumb, quiet ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_width(np.thumb, quiet ? NP_IDLE_GLYPH_W : NP_THUMB);
    lv_obj_set_flex_grow(np.text, quiet ? 0 : 1);
    lv_obj_set_width(np.text, quiet ? LV_SIZE_CONTENT : 0);
    lv_obj_set_width(np.title, quiet ? LV_SIZE_CONTENT : lv_pct(100));
    lv_obj_set_width(np.subtitle, quiet ? LV_SIZE_CONTENT : lv_pct(100));
    lv_obj_set_style_text_opa(np.title, quiet ? UI_OPA_TEXT_MUTED : LV_OPA_COVER, 0);
    ui_set_visible(np.subtitle, !quiet);
    ui_set_visible(lv_obj_get_parent(np.play), !quiet);
}

static void np_card_tint(lv_color_t tint) {
    if (!lv_color_eq(tint, np.tint)) {
        np.tint = tint;
        lv_obj_set_style_bg_color(np.thumb, tint, 0);
    }
}

// With nothing connected the row still says what the device is for.
static void np_card_idle(void) {
    np_card_set_quiet(true);
    ui_set_visible(np.amb_icon, false);
    ui_set_visible(np.art, false);
    ui_set_visible(np.icon, false);
    ui_set_visible(np.idle_icon, true);
    ui_label_set_text(np.title, "Ready to play");
}

// The source's icon, white on the pill's tinted tile while playing, in the
// source's tint on its own when connected but idle.
static void np_card_source_icon(lv_color_t tint, bool on_tile) {
    const lv_image_dsc_t *icon = np_source(np_audio()->wireless_source)->icon;
    if (lv_image_get_src(np.icon) != icon) {
        lv_image_set_src(np.icon, icon);
    }
    lv_color_t color = on_tile ? lv_color_white() : tint;
    if (!lv_color_eq(color, np.icon_color)) {
        np.icon_color = color;
        lv_obj_set_style_image_recolor(np.icon, color, 0);
    }
}

// Like a stream playing, with a stop button.
static void np_card_ambience(void) {
    ui_ambience_t kind;
    bool focus;
    ui_ambience_playing(&kind, &focus);
    np_card_set_quiet(false);
    np_card_tint(lv_color_hex(UI_TINT_AMBIENCE));
    ui_set_visible(np.idle_icon, false);
    ui_set_visible(np.icon, false);
    ui_set_visible(np.art, false);
    ui_set_visible(np.amb_icon, true);
    ui_label_set_text(np.title, ui_ambience_name(kind));
    ui_label_set_text(np.subtitle, focus ? "Focus sound" : "Ambience");
    ui_label_set_text(np.play, LV_SYMBOL_STOP);
}

void np_card_refresh(lv_obj_t *target) {
    if (!target) {
        np_card_idle();
        return;
    }
    if (np_is_ambience(target)) {
        np_card_ambience();
        return;
    }
    const np_audio_t *audio = np_audio();
    ui_set_visible(np.amb_icon, false);
#ifdef AK_USB_AUDIO
    bool usb = target == np_usb_screen();
    bool playing = usb ? audio->usb_playing : audio->wireless_playing;
#else
    bool usb = false;
    bool playing = audio->wireless_playing;
#endif
    const np_source_info_t *source = np_source(audio->wireless_source);
    lv_color_t tint = lv_color_hex(usb ? UI_TINT_USB : source->tint);
    np_card_tint(tint);
    ui_set_visible(np.idle_icon, false);

    // Connected with nothing to resume: which device, quietly.
    if (!playing && (usb || !audio->wireless_has_track)) {
        np_card_set_quiet(true);
        np_card_source_icon(tint, false);
        ui_set_visible(np.art, false);
        ui_set_visible(np.icon, !usb);
        const char *name = audio->wireless_peer[0] ? audio->wireless_peer : source->label;
        ui_label_set_text(np.title, usb ? "USB Audio" : name);
        return;
    }

    np_card_set_quiet(false);
    np_card_source_icon(tint, true);

    // Shares the now-playing screen's copy of the art.
    uint32_t art_version;
    const lv_image_dsc_t *art = np_art(&art_version);
    if (usb) {
        art_version = 0;
    }
    if (art_version != np.art_version) {
        np.art_version = art_version;
        if (art_version) {
            lv_image_set_src(np.art, art);
        }
    }
    ui_set_visible(np.art, art_version != 0);
    ui_set_visible(np.icon, !usb && art_version == 0);

    const char *subtitle = usb ? "" : audio->wireless_subtitle;
    ui_label_set_text(np.title, usb ? "USB Audio" : audio->wireless_text);
    ui_label_set_text(np.subtitle, subtitle[0] ? subtitle : playing ? "Playing" : "Paused");
    ui_label_set_text(np.play, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}
