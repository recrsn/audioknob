#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
// One packet per Ogg page, with the standard OpusHead / OpusTags headers.
size_t note_ogg_headers(uint8_t *out, size_t capacity, uint32_t serial);
size_t note_ogg_packet(uint8_t *out, size_t capacity, uint32_t serial, uint32_t page, uint64_t granule, bool end,
                        const uint8_t *packet, size_t length);
typedef struct {
    size_t offset;
    uint32_t serial, page;
    uint64_t granule;
    uint16_t preskip;
    bool ended;
} note_ogg_reader_t;
bool note_ogg_open(note_ogg_reader_t *reader, const uint8_t *data, size_t size);
// 1 packet, 0 end, -1 malformed. Returned data points into the input buffer.
int note_ogg_next(note_ogg_reader_t *reader, const uint8_t *data, size_t size, const uint8_t **packet,
                   size_t *length);
bool note_ogg_validate(const uint8_t *data, size_t size, uint32_t *duration_ms);

bool note_ogg_mark_end(uint8_t *page, size_t size);
