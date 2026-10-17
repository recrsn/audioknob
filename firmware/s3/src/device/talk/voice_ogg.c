#include "voice_ogg.h"
#include <string.h>
#define OPUS_PACKET_MAX 1275
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t le64(const uint8_t *p) {
    return le32(p) | ((uint64_t)le32(p + 4) << 32);
}
static void put32(uint8_t *p, uint32_t n) {
    for (int i = 0; i < 4; i++) {
        p[i] = n;
        n >>= 8;
    }
}
static void put64(uint8_t *p, uint64_t n) {
    put32(p, n);
    put32(p + 4, n >> 32);
}
static uint32_t crc(const uint8_t *p, size_t n) {
    uint32_t result = 0;
    for (size_t i = 0; i < n; i++) {
        result ^= (uint32_t)((i >= 22 && i < 26) ? 0 : p[i]) << 24;
        for (int b = 0; b < 8; b++)
            result = (result << 1) ^ ((result & 0x80000000) ? 0x04c11db7 : 0);
    }
    return result;
}
static size_t page(uint8_t *out, size_t cap, uint32_t serial, uint32_t seq, uint64_t granule, uint8_t flags,
                   const uint8_t *packet, size_t len) {
    size_t segments = len / 255 + 1, total = 27 + segments + len;
    if (len > OPUS_PACKET_MAX || segments > 255 || cap < total)
        return 0;
    memset(out, 0, 27);
    memcpy(out, "OggS", 4);
    out[5] = flags;
    put64(out + 6, granule);
    put32(out + 14, serial);
    put32(out + 18, seq);
    out[26] = segments;
    size_t remaining = len;
    for (size_t i = 0; i < segments; i++) {
        out[27 + i] = remaining >= 255 ? 255 : remaining;
        remaining -= out[27 + i];
    }
    memcpy(out + 27 + segments, packet, len);
    put32(out + 22, crc(out, total));
    return total;
}
size_t voice_ogg_headers(uint8_t *out, size_t cap, uint32_t serial) {
    uint8_t head[19] = {0};
    memcpy(head, "OpusHead", 8);
    head[8] = 1;
    head[9] = 1;
    head[10] = 0x38;
    head[11] = 1;
    put32(head + 12, 16000); // pre-skip 312 @48 kHz
    size_t a = page(out, cap, serial, 0, 0, 2, head, sizeof(head));
    if (!a)
        return 0;
    uint8_t tags[25] = {0};
    memcpy(tags, "OpusTags", 8);
    put32(tags + 8, 9);
    memcpy(tags + 12, "AudioKnob", 9);
    size_t b = page(out + a, cap - a, serial, 1, 0, 0, tags, sizeof(tags));
    return b ? a + b : 0;
}
size_t voice_ogg_packet(uint8_t *out, size_t cap, uint32_t serial, uint32_t seq, uint64_t granule, bool end,
                        const uint8_t *packet, size_t len) {
    return page(out, cap, serial, seq, granule, end ? 4 : 0, packet, len);
}
static int read_page(voice_ogg_reader_t *r, const uint8_t *data, size_t size, const uint8_t **packet, size_t *length,
                     uint8_t *flags) {
    if (r->offset == size)
        return 0;
    if (r->ended)
        return -1;
    if (size - r->offset < 28)
        return -1;
    const uint8_t *p = data + r->offset;
    if (memcmp(p, "OggS", 4) || p[4] || p[5] & ~6 || !p[26])
        return -1;
    size_t header = 27 + p[26];
    if (header > size - r->offset)
        return -1;
    size_t len = 0;
    for (int i = 0; i < p[26]; i++) {
        if (i < p[26] - 1 && p[27 + i] != 255)
            return -1;
        len += p[27 + i];
    }
    if (p[header - 1] == 255 || len > OPUS_PACKET_MAX || header + len > size - r->offset)
        return -1;
    if (le32(p + 18) != r->page || (r->page && le32(p + 14) != r->serial) || le32(p + 22) != crc(p, header + len))
        return -1;
    if (!r->page)
        r->serial = le32(p + 14);
    uint64_t granule = le64(p + 6);
    if (granule < r->granule)
        return -1;
    r->ended = (p[5] & 4) != 0;
    r->granule = granule;
    r->page++;
    r->offset += header + len;
    *packet = p + header;
    *length = len;
    *flags = p[5];
    return 1;
}
bool voice_ogg_open(voice_ogg_reader_t *r, const uint8_t *data, size_t size) {
    memset(r, 0, sizeof(*r));
    const uint8_t *p;
    size_t n;
    uint8_t flags;
    if (read_page(r, data, size, &p, &n, &flags) != 1 || flags != 2 || n != 19 || memcmp(p, "OpusHead", 8) ||
        p[8] != 1 || p[9] != 1 || p[18] || p[16] || p[17])
        return false;
    r->preskip = p[10] | (p[11] << 8);
    if (read_page(r, data, size, &p, &n, &flags) != 1 || flags || n < 16 || memcmp(p, "OpusTags", 8))
        return false;
    return true;
}
int voice_ogg_next(voice_ogg_reader_t *r, const uint8_t *data, size_t size, const uint8_t **packet, size_t *length) {
    uint8_t flags;
    int rc = read_page(r, data, size, packet, length, &flags);
    if (rc == 1 && ((flags & 2) || !*length || ((flags & 4) && r->offset != size)))
        return -1;
    return rc;
}
bool voice_ogg_validate(const uint8_t *data, size_t size, uint32_t *duration) {
    voice_ogg_reader_t r;
    if (!voice_ogg_open(&r, data, size))
        return false;
    const uint8_t *p;
    size_t len;
    int rc;
    uint32_t frames = 0;
    while ((rc = voice_ogg_next(&r, data, size, &p, &len)) == 1) {
        // Restrict notes to exactly one 20 ms Opus frame per page. TOC
        // duration: SILK configs 0..11 (10/20/40/60), hybrid 12..15
        // (10/20), CELT 16..31 (2.5/5/10/20).
        unsigned cfg = p[0] >> 3, code = p[0] & 3;
        unsigned samples = cfg < 12   ? (cfg % 4 == 3 ? 2880 : 480 << (cfg % 4))
                           : cfg < 16 ? 480 << (cfg % 2)
                                      : 120 << (cfg % 4);
        unsigned count = code == 0 ? 1 : code < 3 ? 2 : len > 1 ? p[1] & 63 : 0;
        if (!count || samples * count != 960 || ++frames > 3000 || r.granule > (uint64_t)frames * 960)
            return false;
    }
    if (rc < 0 || !r.ended || !frames || r.granule <= r.preskip)
        return false;
    *duration = (uint32_t)((r.granule - r.preskip) * 1000 / 48000);
    return true;
}

bool voice_ogg_mark_end(uint8_t *p, size_t size) {
    if (size < 28 || memcmp(p, "OggS", 4) || 27u + p[26] > size)
        return false;
    size_t len = 27 + p[26];
    for (int i = 0; i < p[26]; i++)
        len += p[27 + i];
    if (len != size)
        return false;
    p[5] |= 4;
    put32(p + 22, crc(p, size));
    return true;
}
