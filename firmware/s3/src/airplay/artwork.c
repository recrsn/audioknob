#ifndef AK_NATIVE

#include "artwork.h"

#include <string.h>

#include <stdatomic.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "artwork_png.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// esp_new_jpeg's hardware-friendly scaler only takes multiples of 8, down to
// 1/8; it does the heavy shrink and nearest-neighbour finishes at 132.
#define SCALE_ALIGN 8
#define SCALE_MAX_RATIO 8
#define SCALE_TARGET ((ARTWORK_SIZE + SCALE_ALIGN - 1) / SCALE_ALIGN * SCALE_ALIGN)

static const char *TAG = "artwork";

struct artwork_slot {
    SemaphoreHandle_t lock;
    uint16_t *buf[2];
    int front;
    uint32_t version; // 0 = empty
};

static atomic_uint_fast32_t s_next_version = 1;

static uint16_t round_up(uint16_t v, uint16_t align) {
    return (v + align - 1) / align * align;
}

// Scaled size for one axis: as small as the scaler allows while staying at
// least SCALE_TARGET, or 0 to leave the axis unscaled.
static uint16_t scaled_dim(uint16_t dim) {
    if (dim <= SCALE_TARGET) {
        return 0;
    }
    uint16_t min_allowed = round_up((dim + SCALE_MAX_RATIO - 1) / SCALE_MAX_RATIO, SCALE_ALIGN);
    return min_allowed > SCALE_TARGET ? min_allowed : SCALE_TARGET;
}

static bool read_header(const uint8_t *jpeg, size_t len, jpeg_dec_header_info_t *info) {
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_dec_handle_t dec;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return false;
    }
    jpeg_dec_io_t io = {.inbuf = (uint8_t *)jpeg, .inbuf_len = len};
    bool ok = jpeg_dec_parse_header(dec, &io, info) == JPEG_ERR_OK;
    jpeg_dec_close(dec);
    return ok;
}

// Decodes at (roughly) the output size; returns the bitmap and its size.
static uint16_t *decode_scaled(const uint8_t *jpeg, size_t len, int *w, int *h) {
    jpeg_dec_header_info_t info;
    if (!read_header(jpeg, len, &info)) {
        ESP_LOGW(TAG, "unsupported JPEG header");
        return NULL;
    }

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    uint16_t sw = scaled_dim(info.width), sh = scaled_dim(info.height);
    if (sw || sh) {
        // The scaler needs both axes; keep a small one at its own size.
        cfg.scale.width = sw ? sw : info.width / SCALE_ALIGN * SCALE_ALIGN;
        cfg.scale.height = sh ? sh : info.height / SCALE_ALIGN * SCALE_ALIGN;
    }

    jpeg_dec_handle_t dec;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return NULL;
    }
    uint16_t *pixels = NULL;
    jpeg_dec_io_t io = {.inbuf = (uint8_t *)jpeg, .inbuf_len = len};
    int out_len = 0;
    if (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK && jpeg_dec_get_outbuf_len(dec, &out_len) == JPEG_ERR_OK) {
        // PSRAM, not internal RAM: AirPlay's RTP task needs the latter.
        pixels = heap_caps_aligned_alloc(16, out_len, MALLOC_CAP_SPIRAM);
        io.outbuf = (uint8_t *)pixels;
        jpeg_error_t err = pixels ? jpeg_dec_process(dec, &io) : JPEG_ERR_NO_MEM;
        if (err != JPEG_ERR_OK) {
            ESP_LOGW(TAG, "decode failed (%d)", err);
            heap_caps_free(pixels);
            pixels = NULL;
        }
    }
    jpeg_dec_close(dec);

    *w = cfg.scale.width ? cfg.scale.width : info.width;
    *h = cfg.scale.height ? cfg.scale.height : info.height;
    return pixels;
}

static void resample(const uint16_t *src, int sw, int sh, uint16_t *out) {
    for (int dy = 0; dy < ARTWORK_SIZE; dy++) {
        const uint16_t *row = src + ((dy * sh + sh / 2) / ARTWORK_SIZE) * sw;
        for (int dx = 0; dx < ARTWORK_SIZE; dx++) {
            out[dy * ARTWORK_SIZE + dx] = row[(dx * sw + sw / 2) / ARTWORK_SIZE];
        }
    }
}

artwork_slot_t *artwork_slot_create(void) {
    artwork_slot_t *slot = calloc(1, sizeof(*slot));
    if (!slot) {
        return NULL;
    }
    slot->lock = xSemaphoreCreateMutex();
    for (int i = 0; i < 2; i++) {
        slot->buf[i] = heap_caps_malloc(ARTWORK_SIZE * ARTWORK_SIZE * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }
    return slot;
}

static bool is_png(const uint8_t *data, size_t len) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    return len >= sizeof(sig) && memcmp(data, sig, sizeof(sig)) == 0;
}

static bool decode_jpeg(const uint8_t *jpeg, size_t len, uint16_t *out) {
    int w, h;
    uint16_t *pixels = decode_scaled(jpeg, len, &w, &h);
    if (!pixels) {
        return false;
    }
    resample(pixels, w, h, out);
    heap_caps_free(pixels);
    ESP_LOGI(TAG, "decoded %dx%d from %u bytes", w, h, (unsigned)len);
    return true;
}

bool artwork_slot_set(artwork_slot_t *slot, const uint8_t *image, size_t len) {
    if (!len) {
        artwork_slot_clear(slot);
        return true;
    }

    int back = !slot->front;
    bool ok = is_png(image, len) ? artwork_png_decode(image, len, slot->buf[back])
                                 : decode_jpeg(image, len, slot->buf[back]);
    if (!ok) {
        return false;
    }

    xSemaphoreTake(slot->lock, portMAX_DELAY);
    slot->front = back;
    slot->version = atomic_fetch_add(&s_next_version, 1);
    xSemaphoreGive(slot->lock);
    return true;
}

void artwork_slot_clear(artwork_slot_t *slot) {
    xSemaphoreTake(slot->lock, portMAX_DELAY);
    slot->version = 0;
    xSemaphoreGive(slot->lock);
}

const uint16_t *artwork_slot_get(artwork_slot_t *slot, uint32_t *version) {
    xSemaphoreTake(slot->lock, portMAX_DELAY);
    *version = slot->version;
    const uint16_t *art = slot->version ? slot->buf[slot->front] : NULL;
    xSemaphoreGive(slot->lock);
    return art;
}

#endif  // !AK_NATIVE
