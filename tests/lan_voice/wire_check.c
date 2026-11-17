#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "talk/wire.h"

int main(void) {
    talk_packet_t p = {.ssrc = 0x12345678, .timestamp = 960, .sequence = 65535, .length = 3, .payload = {1, 2, 3}}, q;
    uint8_t wire[1400];
    size_t n = talk_rtp_encode(wire, &p);
    assert(n == 15 && talk_rtp_decode(&q, wire, n));
    assert(q.ssrc == p.ssrc && q.timestamp == 960 && q.sequence == 65535 && !memcmp(q.payload, p.payload, 3));
    // CSRC + extension + padding, all legal RTP forms.
    uint8_t extended[64] = {0xB1, 96, 0, 1, 0, 0, 3, 0, 0, 0, 0, 7, 0, 0, 0, 9, 0, 1, 0, 1, 1, 2, 3, 4, 0x88, 0, 0, 2};
    assert(talk_rtp_decode(&q, extended, 28) && q.length == 2 && q.payload[0] == 0x88);
    extended[27] = 50;
    assert(!talk_rtp_decode(&q, extended, 28));
    talk_jitter_t jitter;
    talk_jitter_init(&jitter, 65534);
    p.sequence = 0;
    assert(talk_jitter_push(&jitter, &p));
    p.sequence = 65534;
    assert(talk_jitter_push(&jitter, &p));
    assert(!talk_jitter_push(&jitter, &p));
    assert(talk_jitter_pop(&jitter, wire) == 3);
    assert(talk_jitter_pop(&jitter, wire) == 0);
    assert(talk_jitter_pop(&jitter, wire) == 3);
    p.sequence = 65533;
    assert(!talk_jitter_push(&jitter, &p));
    p.sequence = jitter.next + 8;
    assert(!talk_jitter_push(&jitter, &p));
    // Random datagrams and truncated headers under ASan/UBSan.
    for (int trial = 0; trial < 50000; trial++) {
        size_t len = rand() % sizeof(wire);
        for (size_t i = 0; i < len; i++)
            wire[i] = rand();
        talk_rtp_decode(&q, wire, len);
    }
    puts("RTP framing and jitter wrap/loss passed");
}
