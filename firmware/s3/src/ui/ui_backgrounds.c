#include "ui_internal.h"

#include <stdlib.h>

#include "ui_bg_data.h"

// The backgrounds are rendered and dithered ahead of time by
// tools/gen_backgrounds.py: LVGL's own gradients quantize straight to RGB565
// and band, and rounded or gradient shapes cost their whole masked area every
// frame. They're copied out of flash once, since a full-screen blit out of
// PSRAM is faster than out of flash.

#define CORNER_DSC(px)                                                                                         \
    {                                                                                                            \
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_ARGB8888, .w = UI_BG_TILE_RADIUS,      \
                   .h = UI_BG_TILE_RADIUS, .stride = UI_BG_TILE_RADIUS * 4},                                     \
        .data_size = UI_BG_CORNER_BYTES, .data = (px),                                                         \
    }
#define CORNERS(name) {CORNER_DSC(name[0]), CORNER_DSC(name[1]), CORNER_DSC(name[2]), CORNER_DSC(name[3])}

static const struct {
    const uint16_t *pixels;
    lv_image_dsc_t corners[4];
} k_now_playing[] = {
    [UI_BG_AIRPLAY] = {ui_bg_airplay, CORNERS(ui_bg_airplay_corners)},
    [UI_BG_BLUETOOTH] = {ui_bg_bluetooth, CORNERS(ui_bg_bluetooth_corners)},
    [UI_BG_USB] = {ui_bg_usb, CORNERS(ui_bg_usb_corners)},
};

// Matching the display's byte order keeps the blit a plain memcpy.
static void load_pixels(lv_image_dsc_t *dsc, const uint16_t *src) {
    bool swap = lv_display_get_color_format(NULL) == LV_COLOR_FORMAT_RGB565_SWAPPED;
    uint16_t *px = (uint16_t *)dsc->data;
    for (size_t i = 0; i < UI_BG_W * UI_BG_H; i++) {
        px[i] = swap ? (uint16_t)(src[i] << 8 | src[i] >> 8) : src[i];
    }
    dsc->header.cf = swap ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565;
}

// Plain malloc, not lv_malloc: ~250 KB won't fit LVGL's pool, and on the S3
// large allocations land in PSRAM. Lives as long as its screen.
static lv_obj_t *fullscreen_create(lv_obj_t *parent, lv_image_dsc_t *dsc, const uint16_t *src) {
    uint16_t *px = malloc(UI_BG_W * UI_BG_H * sizeof(uint16_t));
    if (!px) {
        return NULL;
    }
    *dsc = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .w = UI_BG_W, .h = UI_BG_H, .stride = UI_BG_W * sizeof(uint16_t)},
        .data_size = UI_BG_W * UI_BG_H * sizeof(uint16_t),
        .data = (const uint8_t *)px,
    };
    load_pixels(dsc, src);

    lv_obj_t *img = lv_image_create(parent);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(img, dsc);
    lv_obj_set_pos(img, 0, 0);
    return img;
}

lv_obj_t *ui_wallpaper_create(lv_obj_t *parent) {
    static lv_image_dsc_t dsc;
    return fullscreen_create(parent, &dsc, ui_bg_wallpaper);
}

void ui_now_playing_bg_create(ui_now_playing_bg_t *bg, lv_obj_t *parent, ui_bg_source_t source) {
    bg->source = source;
    bg->img = fullscreen_create(parent, &bg->dsc, k_now_playing[source].pixels);
}

lv_obj_t *ui_now_playing_art_create(ui_now_playing_bg_t *bg, lv_obj_t *parent, int32_t tile_y) {
    lv_obj_t *art = lv_image_create(parent);
    lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(art, LV_ALIGN_CENTER, 0, tile_y);
    lv_obj_add_flag(art, LV_OBJ_FLAG_HIDDEN);

    const int32_t far = UI_BG_TILE_SIZE - UI_BG_TILE_RADIUS;
    for (int i = 0; i < 4; i++) {
        bg->corners[i] = lv_image_create(art);
        lv_obj_remove_flag(bg->corners[i], LV_OBJ_FLAG_CLICKABLE);
        lv_image_set_src(bg->corners[i], &k_now_playing[bg->source].corners[i]);
        lv_obj_set_pos(bg->corners[i], i & 1 ? far : 0, i & 2 ? far : 0);
    }
    return art;
}

void ui_now_playing_bg_set(ui_now_playing_bg_t *bg, ui_bg_source_t source) {
    if (source == bg->source) {
        return;
    }
    bg->source = source;
    if (bg->img) {
        load_pixels(&bg->dsc, k_now_playing[source].pixels);
        lv_image_set_src(bg->img, &bg->dsc);
    }
    for (int i = 0; i < 4; i++) {
        if (bg->corners[i]) {
            lv_image_set_src(bg->corners[i], &k_now_playing[source].corners[i]);
        }
    }
}
