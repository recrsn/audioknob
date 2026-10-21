#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <opus/opus.h>
#include "talk/wire.h"
#include "talk/voice/ogg.h"

int main(int argc, char **argv) {
    assert(argc == 2);
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
    int error;
    OpusEncoder *enc = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, &error);
    assert(enc && error == OPUS_OK);
    assert(opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000)) == OPUS_OK);
    uint8_t ogg[50000], packet[1275];
    size_t used = voice_ogg_headers(ogg, sizeof(ogg), 123), last = 0;
    assert(used);
    for (int frame = 0; frame < 100; frame++) {
        int16_t pcm[320];
        for (int i = 0; i < 320; i++)
            pcm[i] = (int16_t)(5000 * sin((frame * 320 + i) * 2 * 3.141592653589793 * 440 / 16000));
        int len = opus_encode(enc, pcm, 320, packet, sizeof(packet));
        assert(len > 0);
        last = used;
        n = voice_ogg_packet(ogg + used, sizeof(ogg) - used, 123, frame + 2, (frame + 1) * 960, false, packet, len);
        assert(n);
        used += n;
    }
    opus_encoder_destroy(enc);
    assert(voice_ogg_mark_end(ogg + last, used - last));
    uint32_t duration;
    assert(voice_ogg_validate(ogg, used, &duration) && duration == 1993);
    voice_ogg_reader_t reader;
    assert(voice_ogg_open(&reader, ogg, used));
    const uint8_t *data;
    size_t len;
    int count = 0;
    while (voice_ogg_next(&reader, ogg, used, &data, &len) == 1)
        count++;
    assert(count == 100);
    for (size_t cut = 0; cut < used; cut += 13)
        assert(!voice_ogg_validate(ogg, cut, &duration));
    for (size_t i = 0; i < used; i += 31) {
        ogg[i] ^= 0x80;
        assert(!voice_ogg_validate(ogg, used, &duration));
        ogg[i] ^= 0x80;
    }
    FILE *file = fopen(argv[1], "wb");
    assert(file && fwrite(ogg, 1, used, file) == used);
    fclose(file);
    puts("RTP, jitter wrap/loss, Ogg CRC/truncation, and real Opus encoding passed");
}
