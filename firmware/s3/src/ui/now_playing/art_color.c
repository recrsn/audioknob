#include "ui/now_playing/art_color.h"

#include <math.h>

// Every third pixel each way: ~1900 samples of a 132 px cover is plenty for
// a hue histogram and stays well under a millisecond.
#define STEP 3
#define HUE_BUCKETS 24

// Samples darker than this, or with less colour, say nothing about the hue.
#define MIN_MAX     40
#define MIN_CHROMA  32
#define MIN_SAT     0.2f

// Below this share of chroma-weighted samples the art is effectively grey.
#define MIN_WEIGHT_PER_SAMPLE 5

// Bright enough for an accent on UI_COLOR_BG, dark enough that a 24 % wash of
// it still sits under white text.
#define L_MIN 0.58f
#define L_MAX 0.70f
#define S_MIN 0.45f
#define S_MAX 0.90f

typedef struct {
    uint32_t weight;
    uint32_t r, g, b;
} bucket_t;

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static int hue_bucket(int r, int g, int b, int max, int chroma) {
    float h;
    if (max == r) {
        h = (float)(g - b) / chroma;
    } else if (max == g) {
        h = 2.0f + (float)(b - r) / chroma;
    } else {
        h = 4.0f + (float)(r - g) / chroma;
    }
    h = h < 0.0f ? h + 6.0f : h;
    int i = (int)(h * HUE_BUCKETS / 6.0f);
    return i >= HUE_BUCKETS ? HUE_BUCKETS - 1 : i;
}

static float hue_channel(float p, float q, float t) {
    t = t < 0.0f ? t + 1.0f : t > 1.0f ? t - 1.0f : t;
    if (t < 1.0f / 6.0f) {
        return p + (q - p) * 6.0f * t;
    }
    if (t < 0.5f) {
        return q;
    }
    if (t < 2.0f / 3.0f) {
        return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    }
    return p;
}

// Through HSL, so the hue survives while lightness and saturation move.
static uint32_t clamp_readable(float r, float g, float b) {
    float max = fmaxf(r, fmaxf(g, b)), min = fminf(r, fminf(g, b));
    float l = (max + min) / 2.0f, d = max - min;
    float s = d == 0.0f ? 0.0f : d / (1.0f - fabsf(2.0f * l - 1.0f));
    float h = 0.0f;
    if (d > 0.0f) {
        h = max == r ? fmodf((g - b) / d + 6.0f, 6.0f) : max == g ? (b - r) / d + 2.0f : (r - g) / d + 4.0f;
        h /= 6.0f;
    }

    l = clampf(l, L_MIN, L_MAX);
    s = clampf(s, S_MIN, S_MAX);
    float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
    float p = 2.0f * l - q;
    uint32_t out = 0;
    for (int i = 0; i < 3; i++) {
        float c = hue_channel(p, q, h + (1 - i) / 3.0f);
        out = out << 8 | (uint32_t)lroundf(clampf(c, 0.0f, 1.0f) * 255.0f);
    }
    return out;
}

uint32_t art_color_dominant(const uint16_t *pixels, int w, int h) {
    bucket_t buckets[HUE_BUCKETS] = {0};
    uint32_t samples = 0;
    for (int y = STEP / 2; y < h; y += STEP) {
        for (int x = STEP / 2; x < w; x += STEP) {
            uint16_t px = pixels[y * w + x];
            int r = (px >> 11) << 3, g = ((px >> 5) & 0x3f) << 2, b = (px & 0x1f) << 3;
            int max = r > g ? (r > b ? r : b) : (g > b ? g : b);
            int min = r < g ? (r < b ? r : b) : (g < b ? g : b);
            int chroma = max - min;
            samples++;
            if (max < MIN_MAX || chroma < MIN_CHROMA || chroma < MIN_SAT * max) {
                continue;
            }
            // Weighted by chroma: a vivid patch outvotes a wider muted one.
            bucket_t *bk = &buckets[hue_bucket(r, g, b, max, chroma)];
            bk->weight += chroma;
            bk->r += (uint32_t)r * chroma;
            bk->g += (uint32_t)g * chroma;
            bk->b += (uint32_t)b * chroma;
        }
    }

    // Neighbours count too, so a hue straddling two buckets isn't split.
    int best = -1;
    uint32_t best_score = 0;
    for (int i = 0; i < HUE_BUCKETS; i++) {
        uint32_t score = buckets[i].weight * 2 + buckets[(i + 1) % HUE_BUCKETS].weight +
                         buckets[(i + HUE_BUCKETS - 1) % HUE_BUCKETS].weight;
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    if (best < 0) {
        return 0;
    }

    uint64_t weight = 0, r = 0, g = 0, b = 0;
    for (int d = -1; d <= 1; d++) {
        const bucket_t *bk = &buckets[(best + d + HUE_BUCKETS) % HUE_BUCKETS];
        weight += bk->weight;
        r += bk->r;
        g += bk->g;
        b += bk->b;
    }
    if (weight < (uint64_t)samples * MIN_WEIGHT_PER_SAMPLE) {
        return 0;
    }
    return clamp_readable(r / (float)weight / 255.0f, g / (float)weight / 255.0f, b / (float)weight / 255.0f);
}
