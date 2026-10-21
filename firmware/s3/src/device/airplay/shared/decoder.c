#include "decoder.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "decoder/impl/esp_aac_dec.h"
#include "decoder/impl/esp_alac_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "raop.h"

typedef enum {
    CODEC_AAC,
    CODEC_ALAC,
} codec_t;

struct decoder {
    codec_t codec;
    void *handle;
    uint32_t rate;  // AAC's; ALAC's is in the cookie
    uint8_t bits;
    uint32_t frame_length;
    bool format_logged;
    // ALACSpecificConfig: frameLength, compatibleVersion, bitDepth, pb, mb,
    // kb, numChannels, maxRun, maxFrameBytes, avgBitRate, sampleRate
    // (big-endian).
    uint8_t cookie[24];
};

static const char *TAG = "airplay_dec";

// AirPlay 2 doesn't send its ALAC configs: 352-frame stereo, as
// shairport-sync found, 16-bit 44.1 kHz realtime or 24-bit 48 kHz buffered.
static const int s_alac_default[12] = {96, 352, 0, 16, 40, 10, 14, 2, 255, 0, 0, RAOP_SAMPLE_RATE};
static const int s_alac_s24_48k[12] = {96, 352, 0, 24, 40, 10, 14, 2, 255, 0, 0, 48000};

static void put_be(uint8_t *p, uint32_t v, int bytes) {
    for (int i = bytes - 1; i >= 0; i--) {
        p[i] = (uint8_t)v;
        v >>= 8;
    }
}

// The decoder wants ADTS framing; AirPlay sends raw AAC-LC access units.
// Layout per shairport-sync's addADTStoPacket() (MIT License).
static void add_adts(uint8_t *p, size_t total_len, uint32_t rate) {
    const int profile = 2;  // AAC LC
    const int freq_idx = rate == 48000 ? 3 : 4;
    const int chan_cfg = 2;
    p[0] = 0xFF;
    p[1] = 0xF9;
    p[2] = (uint8_t)(((profile - 1) << 6) + (freq_idx << 2) + (chan_cfg >> 2));
    p[3] = (uint8_t)(((chan_cfg & 3) << 6) + (total_len >> 11));
    p[4] = (uint8_t)((total_len & 0x7FF) >> 3);
    p[5] = (uint8_t)(((total_len & 7) << 5) + 0x1F);
    p[6] = 0xFC;
}

// esp_audio_codec allocates through these weak hooks, which default to
// malloc(): its AAC state (~15 KB) would land in internal RAM, already
// short while a buffered stream plays.
void *media_lib_module_malloc(const char *module, size_t size) {
    (void)module;
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

void *media_lib_module_calloc(const char *module, size_t n, size_t size) {
    (void)module;
    return heap_caps_calloc_prefer(n, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_8BIT);
}

void media_lib_free(void *buf) {
    heap_caps_free(buf);
}

static const char *codec_name(const decoder_t *dec) {
    return dec->codec == CODEC_AAC ? "AAC" : "ALAC";
}

static bool open_handle(decoder_t *dec) {
    esp_audio_err_t err;
    if (dec->codec == CODEC_AAC) {
        esp_aac_dec_cfg_t cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
        cfg.sample_rate = (int32_t)dec->rate;
        cfg.channel = 2;
        cfg.bits_per_sample = 16;
        cfg.no_adts_header = false;
        err = esp_aac_dec_open(&cfg, sizeof(cfg), &dec->handle);
    } else {
        esp_alac_dec_cfg_t cfg = {.codec_spec_info = dec->cookie, .spec_info_len = sizeof(dec->cookie)};
        err = esp_alac_dec_open(&cfg, sizeof(cfg), &dec->handle);
    }
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "%s decoder open failed: %d", codec_name(dec), err);
        dec->handle = NULL;
        return false;
    }
    return true;
}

static void close_handle(decoder_t *dec) {
    if (!dec->handle) {
        return;
    }
    if (dec->codec == CODEC_AAC) {
        esp_aac_dec_close(dec->handle);
    } else {
        esp_alac_dec_close(dec->handle);
    }
    dec->handle = NULL;
}

static decoder_t *open_codec(codec_t codec, uint32_t rate, const int *fmtp) {
    decoder_t *dec = calloc(1, sizeof(*dec));
    if (!dec) {
        return NULL;
    }
    dec->codec = codec;
    dec->rate = rate;
    dec->bits = 16;
    if (fmtp) {
        dec->bits = (uint8_t)fmtp[3];
        dec->frame_length = (uint32_t)fmtp[1];
        put_be(&dec->cookie[0], (uint32_t)fmtp[1], 4);
        for (int i = 0; i < 6; i++) {
            dec->cookie[4 + i] = (uint8_t)fmtp[2 + i];
        }
        put_be(&dec->cookie[10], (uint32_t)fmtp[8], 2);
        put_be(&dec->cookie[12], (uint32_t)fmtp[9], 4);
        put_be(&dec->cookie[16], (uint32_t)fmtp[10], 4);
        put_be(&dec->cookie[20], (uint32_t)fmtp[11], 4);
    }
    if (!open_handle(dec)) {
        free(dec);
        return NULL;
    }
    return dec;
}

decoder_t *decoder_open_aac(uint32_t rate) {
    return open_codec(CODEC_AAC, rate, NULL);
}

decoder_t *decoder_open_alac(void) {
    return open_codec(CODEC_ALAC, RAOP_SAMPLE_RATE, s_alac_default);
}

decoder_t *decoder_open_alac_s24_48k(void) {
    return open_codec(CODEC_ALAC, 48000, s_alac_s24_48k);
}

decoder_t *decoder_open_alac_fmtp(const int fmtp[12]) {
    if (fmtp[1] <= 0 || fmtp[1] > DECODER_MAX_FRAMES || (fmtp[3] != 16 && fmtp[3] != 24) || fmtp[7] != 2) {
        return NULL;
    }
    return open_codec(CODEC_ALAC, (uint32_t)fmtp[11], fmtp);
}

// Widens the decoder's output, in place at the start of out, to
// left-justified 32-bit. Back to front, since every sample grows.
static size_t widen(decoder_t *dec, int32_t *out, size_t bytes) {
    uint8_t *raw = (uint8_t *)out;
    if (dec->bits == 16) {
        size_t samples = bytes / 2;
        const int16_t *in = (const int16_t *)raw;
        for (size_t i = samples; i-- > 0;) {
            out[i] = (int32_t)in[i] << 16;
        }
        return samples / 2;
    }
    // 24-bit: esp_audio_codec packs it in 3 bytes, little-endian. A 4-byte
    // container (right-justified) is recognised by its size, in case.
    bool container = dec->frame_length && bytes == dec->frame_length * 2 * 4;
    if (!dec->format_logged) {
        dec->format_logged = true;
        ESP_LOGI(TAG, "ALAC %u-bit: %u bytes per packet, %s samples", dec->bits, (unsigned)bytes,
                 container ? "32-bit" : "packed 24-bit");
    }
    if (container) {
        size_t samples = bytes / 4;
        for (size_t i = 0; i < samples; i++) {
            out[i] = (int32_t)((uint32_t)out[i] << 8);
        }
        return samples / 2;
    }
    size_t samples = bytes / 3;
    for (size_t i = samples; i-- > 0;) {
        const uint8_t *p = raw + i * 3;
        out[i] = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24);
    }
    return samples / 2;
}

void decoder_close(decoder_t *dec) {
    if (dec) {
        close_handle(dec);
        free(dec);
    }
}

int decoder_decode(decoder_t *dec, uint8_t *in, size_t len, int32_t *out, size_t max_frames) {
    if (!dec->handle && !open_handle(dec)) {
        return -1;
    }
    if (dec->codec == CODEC_AAC) {
        in -= DECODER_AAC_HEADROOM;
        len += DECODER_AAC_HEADROOM;
        add_adts(in, len, dec->rate);
    }
    esp_audio_dec_in_raw_t raw = {.buffer = in, .len = (uint32_t)len};
    esp_audio_dec_out_frame_t frame = {
        .buffer = (uint8_t *)out,
        .len = (uint32_t)(max_frames * 2 * sizeof(int32_t)),
    };
    esp_audio_dec_info_t info = {0};
    esp_audio_err_t err = dec->codec == CODEC_AAC ? esp_aac_dec_decode(dec->handle, &raw, &frame, &info)
                                                  : esp_alac_dec_decode(dec->handle, &raw, &frame, &info);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "%s decode error %d, resetting", codec_name(dec), err);
        close_handle(dec);
        return -1;
    }
    // The ALAC decoder leaves info zeroed: its format is the cookie's.
    if ((info.channel && info.channel != 2) || (info.bits_per_sample && info.bits_per_sample != dec->bits)) {
        ESP_LOGW(TAG, "unexpected decoder output: %u ch, %u bit", info.channel, info.bits_per_sample);
        return -1;
    }
    return (int)widen(dec, out, frame.decoded_size);
}
