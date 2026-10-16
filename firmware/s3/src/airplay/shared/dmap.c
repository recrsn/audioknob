#include "dmap.h"

#include <stdint.h>
#include <string.h>

// Each DMAP item: 4-byte ASCII tag, 4-byte big-endian length, payload.
#define DMAP_HEADER_LEN 8

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// Copies at most dst_size-1 bytes without splitting a UTF-8 sequence, so
// LVGL never renders a half glyph at the cut.
static void utf8_copy(char *dst, size_t dst_size, const char *src, size_t src_len) {
    size_t n = src_len < dst_size - 1 ? src_len : dst_size - 1;
    if (n < src_len) {
        while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

bool dmap_parse_track(const char *buf, size_t len, dmap_track_t *out) {
    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *end = p + len;

    while (p < end) {
        if (end - p < DMAP_HEADER_LEN) {
            return false;
        }
        const char *tag = (const char *)p;
        uint32_t item_len = read_be32(p + 4);
        p += DMAP_HEADER_LEN;
        if (item_len > (size_t)(end - p)) {
            return false;
        }

        if (!memcmp(tag, "mlit", 4)) {
            // Descend: the track fields live inside this container.
            end = p + item_len;
            continue;
        }
        if (!memcmp(tag, "minm", 4)) {
            utf8_copy(out->title, sizeof(out->title), (const char *)p, item_len);
        } else if (!memcmp(tag, "asar", 4)) {
            utf8_copy(out->artist, sizeof(out->artist), (const char *)p, item_len);
        } else if (!memcmp(tag, "asal", 4)) {
            utf8_copy(out->album, sizeof(out->album), (const char *)p, item_len);
        }
        p += item_len;
    }
    return true;
}
