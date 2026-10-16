#include "pcm.h"

#include <string.h>

// Per-frame exponential approach: ~3 ms time constant, as AirPlay's player.
#define GAIN_RAMP_DIV 256

void pcm_resampler_init(pcm_resampler_t *rs, uint32_t in_rate, uint32_t out_rate) {
    memset(rs, 0, sizeof(*rs));
    rs->in_rate = in_rate;
    rs->out_rate = out_rate;
    rs->step = ((uint64_t)in_rate << 32) / out_rate;
    // The first output lands on the first input frame, not on `last`.
    rs->pos = 1ULL << 32;
}

size_t pcm_resampler_max_out(const pcm_resampler_t *rs, size_t in_frames) {
    return (size_t)(((uint64_t)(in_frames + 1) << 32) / rs->step) + 1;
}

size_t pcm_resample(pcm_resampler_t *rs, const int16_t *in, size_t in_frames, int16_t *out) {
    if (rs->in_rate == rs->out_rate) {
        memcpy(out, in, in_frames * 2 * sizeof(int16_t));
        return in_frames;
    }
    if (in_frames == 0) {
        return 0;
    }
    // Positions count from `last`, the frame before in[0]: frame k is
    // in[k - 1].
    size_t n = 0;
    const uint64_t end = (uint64_t)in_frames << 32;
    while (rs->pos < end) {
        size_t k = (size_t)(rs->pos >> 32);
        // 15 bits, so a full-scale step times it stays within int32.
        int32_t frac = (int32_t)((rs->pos >> 17) & 0x7FFF);
        const int16_t *a = k ? &in[(k - 1) * 2] : rs->last;
        const int16_t *b = &in[k * 2];
        out[n * 2] = (int16_t)(a[0] + (((b[0] - a[0]) * frac) >> 15));
        out[n * 2 + 1] = (int16_t)(a[1] + (((b[1] - a[1]) * frac) >> 15));
        n++;
        rs->pos += rs->step;
    }
    rs->pos -= end;
    rs->last[0] = in[(in_frames - 1) * 2];
    rs->last[1] = in[(in_frames - 1) * 2 + 1];
    return n;
}

void pcm_to_stereo(int16_t *pcm, size_t frames, int channels) {
    if (channels == 2) {
        return;
    }
    if (channels == 1) {
        // Back to front, so nothing is overwritten before it's read.
        for (size_t i = frames; i-- > 0;) {
            pcm[i * 2] = pcm[i * 2 + 1] = pcm[i];
        }
        return;
    }
    for (size_t i = 0; i < frames; i++) {
        pcm[i * 2] = pcm[i * channels];
        pcm[i * 2 + 1] = pcm[i * channels + 1];
    }
}

int32_t pcm_gain_for_volume(uint8_t volume_pct) {
    float v = (volume_pct > 100 ? 100 : volume_pct) / 100.0f;
    return (int32_t)(v * v * v * PCM_GAIN_UNITY);
}

void pcm_apply_gain(int16_t *pcm, size_t frames, int32_t *gain, int32_t target) {
    int32_t g = *gain;
    for (size_t i = 0; i < frames; i++) {
        if (g != target) {
            int32_t step = (target - g) / GAIN_RAMP_DIV;
            g += step ? step : (target > g ? 1 : -1);
        }
        pcm[i * 2] = (int16_t)((pcm[i * 2] * g) >> 15);
        pcm[i * 2 + 1] = (int16_t)((pcm[i * 2 + 1] * g) >> 15);
    }
    *gain = g;
}
