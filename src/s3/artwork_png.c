#include "artwork_png.h"

#include <png.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "artwork_png";

typedef struct {
    const uint8_t *data;
    size_t len, pos;
} png_source_t;

// Box-filter state. Source pixels are summed into bins -- ARTWORK_SIZE per
// axis, or one per source pixel when the cover is smaller -- so a cover of
// any size needs only one source row in memory.
typedef struct {
    uint32_t sum[ARTWORK_SIZE][3];
    uint32_t count[ARTWORK_SIZE];
    uint16_t *bin_of;  // source column -> column bin
    uint32_t col_bins, row_bins;
} box_t;

static void read_cb(png_structp png, png_bytep out, size_t len) {
    png_source_t *src = png_get_io_ptr(png);
    if (len > src->len - src->pos) {
        png_error(png, "truncated");
    }
    memcpy(out, src->data + src->pos, len);
    src->pos += len;
}

static void error_cb(png_structp png, png_const_charp msg) {
    ESP_LOGW(TAG, "%s", msg);
    png_longjmp(png, 1);
}

static void warning_cb(png_structp png, png_const_charp msg) {
    (void)png;
    (void)msg;
}

// libpng's own allocations (zlib's 32 KB window among them) go to PSRAM:
// AirPlay's RTP task needs the internal RAM.
static png_voidp malloc_cb(png_structp png, png_alloc_size_t size) {
    (void)png;
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

static void free_cb(png_structp png, png_voidp ptr) {
    (void)png;
    heap_caps_free(ptr);
}

static uint32_t bins_for(uint32_t dim) {
    return dim < ARTWORK_SIZE ? dim : ARTWORK_SIZE;
}

// Writes the finished row bin to every output row it covers.
static void emit_rows(box_t *box, uint32_t row_bin, uint16_t *out) {
    uint16_t px[ARTWORK_SIZE];
    for (uint32_t bin = 0; bin < box->col_bins; bin++) {
        uint32_t n = box->count[bin] ? box->count[bin] : 1;
        uint32_t r = (box->sum[bin][0] + n / 2) / n;
        uint32_t g = (box->sum[bin][1] + n / 2) / n;
        uint32_t b = (box->sum[bin][2] + n / 2) / n;
        px[bin] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
    }
    for (int dy = 0; dy < ARTWORK_SIZE; dy++) {
        if ((uint32_t)dy * box->row_bins / ARTWORK_SIZE != row_bin) {
            continue;
        }
        for (int dx = 0; dx < ARTWORK_SIZE; dx++) {
            out[dy * ARTWORK_SIZE + dx] = px[(uint32_t)dx * box->col_bins / ARTWORK_SIZE];
        }
    }
    memset(box->sum, 0, sizeof(box->sum));
    memset(box->count, 0, sizeof(box->count));
}

static void read_scaled(png_structp png, uint32_t w, uint32_t h, uint8_t *row, box_t *box, uint16_t *out) {
    for (uint32_t y = 0; y < h; y++) {
        png_read_row(png, row, NULL);
        for (uint32_t x = 0; x < w; x++) {
            uint32_t bin = box->bin_of[x];
            box->sum[bin][0] += row[x * 3];
            box->sum[bin][1] += row[x * 3 + 1];
            box->sum[bin][2] += row[x * 3 + 2];
            box->count[bin]++;
        }
        uint32_t row_bin = (uint32_t)((uint64_t)y * box->row_bins / h);
        if (y + 1 == h || (uint64_t)(y + 1) * box->row_bins / h != row_bin) {
            emit_rows(box, row_bin, out);
        }
    }
}

bool artwork_png_decode(const uint8_t *data, size_t len, uint16_t *out) {
    png_source_t src = {.data = data, .len = len};
    png_structp png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, NULL, error_cb, warning_cb, NULL, malloc_cb,
                                               free_cb);
    png_infop info = png ? png_create_info_struct(png) : NULL;
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        return false;
    }

    // Volatile: modified between setjmp and a possible longjmp.
    uint8_t *volatile row = NULL;
    box_t *volatile box = NULL;
    volatile bool ok = false;
    if (setjmp(png_jmpbuf(png)) == 0) {
        png_set_read_fn(png, &src, read_cb);
        png_read_info(png, info);
        uint32_t w = png_get_image_width(png, info);
        uint32_t h = png_get_image_height(png, info);
        if (png_get_interlace_type(png, info) != PNG_INTERLACE_NONE) {
            // Adam7 needs the whole image in memory to deinterlace.
            png_error(png, "interlaced PNG not supported");
        }

        // Everything to 8-bit RGB; alpha is dropped, covers are opaque.
        png_set_expand(png);
        png_set_strip_16(png);
        png_set_gray_to_rgb(png);
        png_set_strip_alpha(png);
        png_read_update_info(png, info);

        row = heap_caps_malloc(png_get_rowbytes(png, info), MALLOC_CAP_SPIRAM);
        box = heap_caps_calloc(1, sizeof(box_t), MALLOC_CAP_SPIRAM);
        uint16_t *bin_of = box ? heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_SPIRAM) : NULL;
        if (!row || !bin_of) {
            heap_caps_free(bin_of);
            png_error(png, "out of memory");
        }
        box->bin_of = bin_of;
        box->col_bins = bins_for(w);
        box->row_bins = bins_for(h);
        for (uint32_t x = 0; x < w; x++) {
            box->bin_of[x] = (uint16_t)((uint64_t)x * box->col_bins / w);
        }
        read_scaled(png, w, h, row, box, out);
        ok = true;
        ESP_LOGI(TAG, "decoded %ux%u from %u bytes", (unsigned)w, (unsigned)h, (unsigned)len);
    }

    if (box) {
        heap_caps_free(box->bin_of);
    }
    heap_caps_free(box);
    heap_caps_free(row);
    png_destroy_read_struct(&png, &info, NULL);
    return ok;
}
