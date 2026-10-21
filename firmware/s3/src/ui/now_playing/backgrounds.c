#include "ui/internal.h"

#include <stdlib.h>

#include "ui/assets/ui_bg_data.h"

// The backgrounds are rendered and dithered ahead of time by
// tools/gen_backgrounds.py: LVGL's own gradients quantize straight to RGB565
// and band, and rounded or gradient shapes cost their whole masked area every
// frame. They're copied out of flash once, since a full-screen blit out of
// PSRAM is faster than out of flash.
//
// A now-playing background follows any tint (the cover art's colour): the
// script bakes only how far each pixel goes from UI_COLOR_BG towards the
// tint, and a tint change mixes and dithers that into the PSRAM copy once,
// through a lookup table, rather than drawing anything per frame.

#define TINT_FADE_MS 400

static const uint8_t k_bayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

static const struct {
    const uint8_t *alpha;
    const uint8_t (*opacity)[UI_BG_CORNER_PX];
    const uint8_t (*wash)[UI_BG_CORNER_PX];
    int32_t tile_y;
} k_layouts[] = {
    [UI_BG_WIRELESS] = {ui_bg_wireless_alpha, ui_bg_wireless_corner_opacity, ui_bg_wireless_corner_wash,
                        UI_BG_WIRELESS_TILE_Y},
#ifdef AK_USB_AUDIO
    [UI_BG_USB] = {ui_bg_usb_alpha, ui_bg_usb_corner_opacity, ui_bg_usb_corner_wash, UI_BG_USB_TILE_Y},
#endif
};

static bool display_swapped(void) {
    return lv_display_get_color_format(NULL) == LV_COLOR_FORMAT_RGB565_SWAPPED;
}

static uint8_t channel(uint32_t rgb, int shift) {
    return (uint8_t)(rgb >> shift);
}

static uint32_t tint_mix(uint32_t from, uint32_t to, int32_t t) {
    uint32_t out = 0;
    for (int shift = 16; shift >= 0; shift -= 8) {
        int32_t a = channel(from, shift), b = channel(to, shift);
        out |= (uint32_t)(a + (b - a) * t / 255) << shift;
    }
    return out;
}

// One dithered RGB565 pixel per (Bayer cell, alpha): gen_backgrounds.py's
// dither565() of UI_COLOR_BG mixed towards the tint, so a recolour costs a
// lookup per pixel. Shared, since recolouring only happens on the LVGL task.
static const uint16_t *lut_build(uint32_t tint) {
    static uint16_t *lut;
    if (!lut && !(lut = lv_malloc(16 * 256 * sizeof(uint16_t)))) {
        return NULL;
    }
    static const int levels[3] = {32, 64, 32};
    bool swap = display_swapped();
    for (int cell = 0; cell < 16; cell++) {
        float th = (k_bayer4[cell] + 0.5f) / 16.0f;
        for (int a = 0; a < 256; a++) {
            uint16_t px = 0;
            for (int c = 0; c < 3; c++) {
                int shift = 16 - 8 * c;
                float from = channel(UI_COLOR_BG, shift), to = channel(tint, shift);
                float v = from + (to - from) * a / 255.0f;
                int q = (int)(v * (levels[c] - 1) / 255.0f + th);
                q = q > levels[c] - 1 ? levels[c] - 1 : q;
                px = (uint16_t)(px << (c == 1 ? 6 : 5) | q);
            }
            lut[cell * 256 + a] = swap ? (uint16_t)(px << 8 | px >> 8) : px;
        }
    }
    return lut;
}

static void recolour_pixels(ui_now_playing_bg_t *bg, const uint16_t *lut) {
    const uint8_t *alpha = k_layouts[bg->layout].alpha;
    uint16_t *px = (uint16_t *)bg->dsc.data;
    for (int y = 0; y < UI_BG_H; y++) {
        const uint16_t *row_lut = lut + (y & 3) * 4 * 256;
        for (int x = 0; x < UI_BG_W; x++) {
            *px++ = row_lut[(x & 3) * 256 + *alpha++];
        }
    }
}

// Where an overlay pixel is wholly outside the tile it repeats the
// background's dithered pixel exactly; elsewhere it carries the plain wash.
static void recolour_corners(ui_now_playing_bg_t *bg, uint32_t tint) {
    const uint16_t *px = (const uint16_t *)bg->dsc.data;
    bool swap = display_swapped();
    const int32_t ox = (UI_BG_W - UI_BG_TILE_SIZE) / 2;
    const int32_t oy = (UI_BG_H - UI_BG_TILE_SIZE) / 2 + k_layouts[bg->layout].tile_y;
    const int32_t far = UI_BG_TILE_SIZE - UI_BG_TILE_RADIUS;
    for (int q = 0; q < 4; q++) {
        const uint8_t *opacity = k_layouts[bg->layout].opacity[q];
        const uint8_t *wash = k_layouts[bg->layout].wash[q];
        uint8_t *out = bg->corner_px + q * UI_BG_CORNER_PX * 4;
        for (int i = 0; i < UI_BG_CORNER_PX; i++, out += 4) {
            uint32_t rgb;
            if (opacity[i] == 255) {
                int32_t x = ox + (q & 1 ? far : 0) + i % UI_BG_TILE_RADIUS;
                int32_t y = oy + (q & 2 ? far : 0) + i / UI_BG_TILE_RADIUS;
                uint16_t v = px[y * UI_BG_W + x];
                v = swap ? (uint16_t)(v << 8 | v >> 8) : v;
                uint32_t r = v >> 11, g = (v >> 5) & 0x3f, b = v & 0x1f;
                rgb = (r << 3 | r >> 2) << 16 | (g << 2 | g >> 4) << 8 | (b << 3 | b >> 2);
            } else {
                rgb = tint_mix(UI_COLOR_BG, tint, wash[i]);
            }
            out[0] = channel(rgb, 0);
            out[1] = channel(rgb, 8);
            out[2] = channel(rgb, 16);
            out[3] = opacity[i];
        }
    }
}

static void tint_apply(ui_now_playing_bg_t *bg, uint32_t tint) {
    bg->tint = tint;
    const uint16_t *lut = bg->img ? lut_build(tint) : NULL;
    if (lut) {
        recolour_pixels(bg, lut);
        lv_obj_invalidate(bg->img);
        if (bg->corner_px) {
            recolour_corners(bg, tint);
            lv_obj_invalidate(lv_obj_get_parent(bg->corners[0]));
        }
    }
    if (bg->on_tint) {
        bg->on_tint(tint);
    }
}

static void tint_anim_cb(void *var, int32_t t) {
    ui_now_playing_bg_t *bg = var;
    uint32_t tint = tint_mix(bg->from, bg->to, t);
    if (tint != bg->tint) {
        tint_apply(bg, tint);
    }
}

// Matching the display's byte order keeps the blit a plain memcpy.
static void load_pixels(lv_image_dsc_t *dsc, const uint16_t *src) {
    bool swap = display_swapped();
    uint16_t *px = (uint16_t *)dsc->data;
    for (size_t i = 0; i < UI_BG_W * UI_BG_H; i++) {
        px[i] = swap ? (uint16_t)(src[i] << 8 | src[i] >> 8) : src[i];
    }
}

// Plain malloc, not lv_malloc: ~250 KB won't fit LVGL's pool, and on the S3
// large allocations land in PSRAM. Lives as long as its screen. src NULL
// leaves the pixels for the caller to fill.
static lv_obj_t *fullscreen_create(lv_obj_t *parent, lv_image_dsc_t *dsc, const uint16_t *src) {
    uint16_t *px = malloc(UI_BG_W * UI_BG_H * sizeof(uint16_t));
    if (!px) {
        return NULL;
    }
    *dsc = (lv_image_dsc_t){
        .header = {.magic = LV_IMAGE_HEADER_MAGIC,
                   .cf = display_swapped() ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565,
                   .w = UI_BG_W,
                   .h = UI_BG_H,
                   .stride = UI_BG_W * sizeof(uint16_t)},
        .data_size = UI_BG_W * UI_BG_H * sizeof(uint16_t),
        .data = (const uint8_t *)px,
    };
    if (src) {
        load_pixels(dsc, src);
    }

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

void ui_now_playing_bg_create(ui_now_playing_bg_t *bg, lv_obj_t *parent, ui_bg_layout_t layout, uint32_t tint,
                              void (*on_tint)(uint32_t tint)) {
    *bg = (ui_now_playing_bg_t){.layout = layout, .to = tint, .on_tint = on_tint};
    bg->img = fullscreen_create(parent, &bg->dsc, NULL);
    tint_apply(bg, tint);
}

lv_obj_t *ui_now_playing_art_create(ui_now_playing_bg_t *bg, lv_obj_t *parent, int32_t tile_y) {
    lv_obj_t *art = lv_image_create(parent);
    lv_obj_remove_flag(art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(art, LV_ALIGN_CENTER, 0, tile_y);
    lv_obj_add_flag(art, LV_OBJ_FLAG_HIDDEN);

    // Without a background to repeat there's nothing for the corners to show.
    bg->corner_px = bg->img ? lv_malloc(4 * UI_BG_CORNER_PX * 4) : NULL;
    if (!bg->corner_px) {
        return art;
    }
    recolour_corners(bg, bg->tint);
    const int32_t far = UI_BG_TILE_SIZE - UI_BG_TILE_RADIUS;
    for (int i = 0; i < 4; i++) {
        bg->corner_dsc[i] = (lv_image_dsc_t){
            .header = {.magic = LV_IMAGE_HEADER_MAGIC,
                       .cf = LV_COLOR_FORMAT_ARGB8888,
                       .w = UI_BG_TILE_RADIUS,
                       .h = UI_BG_TILE_RADIUS,
                       .stride = UI_BG_TILE_RADIUS * 4},
            .data_size = UI_BG_CORNER_PX * 4,
            .data = bg->corner_px + i * UI_BG_CORNER_PX * 4,
        };
        bg->corners[i] = lv_image_create(art);
        lv_obj_remove_flag(bg->corners[i], LV_OBJ_FLAG_CLICKABLE);
        lv_image_set_src(bg->corners[i], &bg->corner_dsc[i]);
        lv_obj_set_pos(bg->corners[i], i & 1 ? far : 0, i & 2 ? far : 0);
    }
    return art;
}

void ui_now_playing_bg_set_tint(ui_now_playing_bg_t *bg, uint32_t tint, bool animate) {
    if (tint == bg->to) {
        return;
    }
    lv_anim_delete(bg, tint_anim_cb);
    bg->from = bg->tint;
    bg->to = tint;
    if (!animate || !bg->img) {
        tint_apply(bg, tint);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bg);
    lv_anim_set_exec_cb(&a, tint_anim_cb);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, TINT_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}
