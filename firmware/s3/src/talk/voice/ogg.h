#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// One packet per Ogg page, with the standard OpusHead / OpusTags headers.
size_t voice_ogg_headers(uint8_t *out, size_t capacity, uint32_t serial);
size_t voice_ogg_packet(uint8_t *out, size_t capacity, uint32_t serial, uint32_t page, uint64_t granule, bool end,
                        const uint8_t *packet, size_t length);
typedef struct {
    size_t offset;
    uint32_t serial, page;
    uint64_t granule;
    uint16_t preskip;
    bool ended;
} voice_ogg_reader_t;
bool voice_ogg_open(voice_ogg_reader_t *reader, const uint8_t *data, size_t size);
// 1 packet, 0 end, -1 malformed. Returned data points into the input buffer.
int voice_ogg_next(voice_ogg_reader_t *reader, const uint8_t *data, size_t size, const uint8_t **packet,
                   size_t *length);
bool voice_ogg_validate(const uint8_t *data, size_t size, uint32_t *duration_ms);

bool voice_ogg_mark_end(uint8_t *page, size_t size);
