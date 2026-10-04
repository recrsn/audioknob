#include "artwork.h"

#include <string.h>

#include "esp_log.h"
#include "esp32s3/rom/tjpgd.h"

// ROM tjpgd's documented minimum work area is 3100 bytes.
#define POOL_SIZE 4096

static const char *TAG = "artwork";

typedef struct {
    const uint8_t *data;
    size_t len, pos;
    uint16_t *out;
    // Source pixel feeding each output column/row, after JPEG descaling.
    uint16_t src_x[ARTWORK_SIZE], src_y[ARTWORK_SIZE];
} decode_ctx_t;

static UINT read_cb(JDEC *jd, BYTE *buf, UINT n) {
    decode_ctx_t *ctx = jd->device;
    size_t avail = ctx->len - ctx->pos;
    if (n > avail) {
        n = avail;
    }
    if (buf) {
        memcpy(buf, ctx->data + ctx->pos, n);
    }
    ctx->pos += n;
    return n;
}

// Nearest-neighbour: copy only the source pixels some output pixel samples.
static UINT write_cb(JDEC *jd, void *bitmap, JRECT *rect) {
    decode_ctx_t *ctx = jd->device;
    const uint8_t *rgb = bitmap;
    int stride = rect->right - rect->left + 1;

    for (int dy = 0; dy < ARTWORK_SIZE; dy++) {
        int sy = ctx->src_y[dy];
        if (sy < rect->top || sy > rect->bottom) {
            continue;
        }
        const uint8_t *row = rgb + (sy - rect->top) * stride * 3;
        for (int dx = 0; dx < ARTWORK_SIZE; dx++) {
            int sx = ctx->src_x[dx];
            if (sx < rect->left || sx > rect->right) {
                continue;
            }
            const uint8_t *p = row + (sx - rect->left) * 3;
            ctx->out[dy * ARTWORK_SIZE + dx] = ((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3);
        }
    }
    return 1;
}

bool artwork_decode(const uint8_t *jpeg, size_t len, uint16_t out[ARTWORK_SIZE * ARTWORK_SIZE]) {
    static uint8_t pool[POOL_SIZE];
    static decode_ctx_t ctx;
    JDEC jd;

    ctx = (decode_ctx_t){.data = jpeg, .len = len, .out = out};
    JRESULT res = jd_prepare(&jd, read_cb, pool, sizeof(pool), &ctx);
    if (res != JDR_OK) {
        ESP_LOGW(TAG, "unsupported JPEG (%d)", res);
        return false;
    }

    // Descale as far as possible while staying at least as large as the output.
    int scale = 0;
    while (scale < 3 && (jd.width >> (scale + 1)) >= ARTWORK_SIZE && (jd.height >> (scale + 1)) >= ARTWORK_SIZE) {
        scale++;
    }
    unsigned sw = jd.width >> scale, sh = jd.height >> scale;
    for (int i = 0; i < ARTWORK_SIZE; i++) {
        ctx.src_x[i] = (i * sw + sw / 2) / ARTWORK_SIZE;
        ctx.src_y[i] = (i * sh + sh / 2) / ARTWORK_SIZE;
    }

    res = jd_decomp(&jd, write_cb, scale);
    if (res != JDR_OK) {
        ESP_LOGW(TAG, "decode failed (%d)", res);
        return false;
    }
    return true;
}
