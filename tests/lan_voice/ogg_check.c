#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "talk/note/ogg.h"

// A note as audio.c records it: headers, one 20 ms CELT packet per page,
// granules at 48 kHz, the last page marked once recording stops.
static uint8_t note[64 * 1024];

static size_t record(int frames, uint8_t toc, bool mark) {
    size_t used = note_ogg_headers(note, sizeof(note), 77), last = 0;
    assert(used);
    for (int i = 0; i < frames; i++) {
        uint8_t packet[40] = {toc};
        memset(packet + 1, i, sizeof(packet) - 1);
        last = used;
        size_t n = note_ogg_packet(note + used, sizeof(note) - used, 77, i + 2, (uint64_t)(i + 1) * 960, false, packet,
                                   sizeof(packet));
        assert(n);
        used += n;
    }
    if (mark)
        assert(note_ogg_mark_end(note + last, used - last));
    return used;
}

int main(void) {
    const uint8_t celt20 = 31 << 3, celt10 = 30 << 3;
    uint32_t ms = 0;

    size_t size = record(100, celt20, true);
    assert(note_ogg_validate(note, size, &ms));
    // 2 s less the 6.5 ms pre-skip.
    assert(ms >= 1990 && ms < 2000);

    note_ogg_reader_t r;
    assert(note_ogg_open(&r, note, size));
    const uint8_t *packet;
    size_t length;
    int frames = 0;
    while (note_ogg_next(&r, note, size, &packet, &length) == 1) {
        assert(length == 40 && packet[0] == celt20 && packet[1] == (uint8_t)frames);
        frames++;
    }
    assert(frames == 100 && r.ended);

    // Truncated, corrupted, never finished, or not 20 ms frames: refused.
    assert(!note_ogg_validate(note, size - 10, &ms));
    note[size - 5] ^= 1;
    assert(!note_ogg_validate(note, size, &ms));
    size = record(100, celt20, false);
    assert(!note_ogg_validate(note, size, &ms));
    size = record(100, celt10, true);
    assert(!note_ogg_validate(note, size, &ms));
    assert(!note_ogg_validate((const uint8_t *)"OggS", 4, &ms));
    puts("Voice note Ogg write/read/validate passed");
}
