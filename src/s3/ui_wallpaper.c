#include "ui_internal.h"

#include <math.h>
#include <stdlib.h>

// Ambient wallpaper glow (mock-react's .idle-art): the mock layers several
// blurred `radial-gradient(circle at X% Y%, hsla(...), transparent 40%)`
// blobs under a 50% black scrim. Drawn live by LVGL those quantize straight
// to RGB565 -- 32 levels of red/blue across a dark glow -- and band visibly,
// and LVGL 9 has no gradient dithering. The wallpaper never changes, so it is
// composited once here in float and ordered-dithered down to RGB565.

typedef struct {
    float cx, cy, radius;
    uint32_t rgb;
    float peak_opa;
} glow_blob_t;

typedef struct {
    float r, g, b;
} rgbf_t;

#define WALLPAPER_BG    0x0b0b10
#define SCRIM_KEEP      0.5f

static const uint8_t s_bayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static rgbf_t rgbf_from_hex(uint32_t hex) {
    return (rgbf_t){(float)((hex >> 16) & 0xff), (float)((hex >> 8) & 0xff), (float)(hex & 0xff)};
}

// Eased (1 - smoothstep) rather than linear: a linear ramp ends in a hard
// slope break at the rim that reads as a ring even when dithered.
static float glow_falloff(float dist, float radius) {
    float t = dist / radius;
    if (t >= 1.0f) {
        return 0.0f;
    }
    return 1.0f - t * t * (3.0f - 2.0f * t);
}

static rgbf_t composite_pixel(const glow_blob_t *blobs, size_t count, float x, float y) {
    rgbf_t c = rgbf_from_hex(WALLPAPER_BG);
    for (size_t i = 0; i < count; i++) {
        float a = blobs[i].peak_opa * glow_falloff(hypotf(x - blobs[i].cx, y - blobs[i].cy), blobs[i].radius);
        if (a <= 0.0f) {
            continue;
        }
        rgbf_t col = rgbf_from_hex(blobs[i].rgb);
        c.r += (col.r - c.r) * a;
        c.g += (col.g - c.g) * a;
        c.b += (col.b - c.b) * a;
    }
    c.r *= SCRIM_KEEP;
    c.g *= SCRIM_KEEP;
    c.b *= SCRIM_KEEP;
    return c;
}

// `threshold` in [0, 1) picks whether the fractional part of the scaled
// channel rounds up, so the average over the Bayer cell matches the input.
static uint16_t quantize_channel(float v8, int levels, float threshold) {
    float scaled = v8 * (float)(levels - 1) / 255.0f;
    int q = (int)(scaled + threshold);
    return (uint16_t)(q < 0 ? 0 : q >= levels ? levels - 1 : q);
}

static uint16_t dither_rgb565(rgbf_t c, int x, int y) {
    float threshold = ((float)s_bayer4[y & 3][x & 3] + 0.5f) / 16.0f;
    uint16_t r = quantize_channel(c.r, 32, threshold);
    uint16_t g = quantize_channel(c.g, 64, threshold);
    uint16_t b = quantize_channel(c.b, 32, threshold);
    return (uint16_t)((r << 11) | (g << 5) | b);
}

lv_obj_t *ui_wallpaper_create(lv_obj_t *parent) {
    // Plain malloc, not lv_malloc: ~250 KB won't fit LVGL's pool, and on the
    // S3 large allocations land in PSRAM. Lives as long as the idle screen.
    static lv_image_dsc_t dsc;
    int32_t w = lv_display_get_horizontal_resolution(NULL);
    int32_t h = lv_display_get_vertical_resolution(NULL);
    uint16_t *px = malloc((size_t)w * h * sizeof(uint16_t));
    if (!px) {
        return NULL;
    }

    const glow_blob_t blobs[] = {
        {60.0f, 60.0f, 160.0f, 0x4ea1ff, 0.70f},
        {(float)w - 70.0f, (float)h - 70.0f, 150.0f, 0x7c6cff, 0.60f},
        {(float)w - 60.0f, 60.0f, 130.0f, 0xd946a8, 0.60f},
        {60.0f, (float)h - 60.0f, 130.0f, 0x2dd4a0, 0.50f},
    };
    for (int32_t y = 0; y < h; y++) {
        for (int32_t x = 0; x < w; x++) {
            rgbf_t c = composite_pixel(blobs, sizeof(blobs) / sizeof(blobs[0]), (float)x + 0.5f, (float)y + 0.5f);
            px[y * w + x] = dither_rgb565(c, x, y);
        }
    }

    dsc = (lv_image_dsc_t){
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_RGB565,
            .w = w,
            .h = h,
            .stride = w * sizeof(uint16_t),
        },
        .data_size = (uint32_t)(w * h * sizeof(uint16_t)),
        .data = (const uint8_t *)px,
    };

    lv_obj_t *img = lv_image_create(parent);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(img, &dsc);
    lv_obj_set_pos(img, 0, 0);
    return img;
}
