#include "alac_dec.h"

#include <new>

#include "ALACBitUtilities.h"
#include "ALACDecoder.h"

struct alac_dec {
    ALACDecoder decoder;
    uint32_t frame_length;
    uint32_t channels;
};

static void put_be16(uint8_t *p, uint32_t v) {
    p[0] = v >> 8;
    p[1] = v;
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

extern "C" alac_dec_t *alac_dec_create(const int fmtp[12]) {
    // ALACSpecificConfig, big-endian, exactly as ALACDecoder::Init expects.
    uint8_t cookie[24];
    put_be32(&cookie[0], fmtp[1]);
    cookie[4] = fmtp[2];
    cookie[5] = fmtp[3];
    cookie[6] = fmtp[4];
    cookie[7] = fmtp[5];
    cookie[8] = fmtp[6];
    cookie[9] = fmtp[7];
    put_be16(&cookie[10], fmtp[8]);
    put_be32(&cookie[12], fmtp[9]);
    put_be32(&cookie[16], fmtp[10]);
    put_be32(&cookie[20], fmtp[11]);

    alac_dec *dec = new (std::nothrow) alac_dec;
    if (!dec) {
        return nullptr;
    }
    // Downstream (RTP buffers, the player) is fixed at 16-bit stereo, which
    // is all AirPlay 1 senders ever negotiate.
    if (dec->decoder.Init(cookie, sizeof(cookie)) != ALAC_noErr || fmtp[3] != 16 || fmtp[7] != 2) {
        delete dec;
        return nullptr;
    }
    dec->frame_length = fmtp[1];
    dec->channels = fmtp[7];
    return dec;
}

extern "C" void alac_dec_delete(alac_dec_t *dec) {
    delete dec;
}

extern "C" bool alac_dec_decode(alac_dec_t *dec, uint8_t *in, size_t len, int16_t *out, uint32_t *out_frames) {
    BitBuffer bits;
    BitBufferInit(&bits, in, len);
    return dec->decoder.Decode(&bits, reinterpret_cast<uint8_t *>(out), dec->frame_length, dec->channels, out_frames) ==
           ALAC_noErr;
}
