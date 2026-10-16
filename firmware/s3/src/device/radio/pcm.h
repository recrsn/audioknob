#pragma once

#include <stddef.h>
#include <stdint.h>

// Interleaved s16 stereo helpers for streams whose format isn't the DAC's.

// Linear-interpolation rate converter, continuous across calls.
typedef struct {
    uint32_t in_rate;
    uint32_t out_rate;
    uint64_t step; // input frames per output frame, 32.32
    uint64_t pos;  // next output's position past `last`, 32.32
    int16_t last[2];
} pcm_resampler_t;

void pcm_resampler_init(pcm_resampler_t *rs, uint32_t in_rate, uint32_t out_rate);

// Output frames `in_frames` input frames can produce, at most.
size_t pcm_resampler_max_out(const pcm_resampler_t *rs, size_t in_frames);

// Converts `in_frames` frames into out (room for pcm_resampler_max_out());
// returns frames written.
size_t pcm_resample(pcm_resampler_t *rs, const int16_t *in, size_t in_frames, int16_t *out);

// Interleaves `frames` frames of `channels` channels into stereo, in place
// (mono is duplicated, extra channels dropped). `pcm` must hold
// 2 * frames samples.
void pcm_to_stereo(int16_t *pcm, size_t frames, int channels);

// Software volume: a cubic (roughly perceptual) curve of 0-100 %, ramped per
// frame from `*gain` towards the target so steps don't click.
#define PCM_GAIN_UNITY 32768
int32_t pcm_gain_for_volume(uint8_t volume_pct);
void pcm_apply_gain(int16_t *pcm, size_t frames, int32_t *gain, int32_t target);
