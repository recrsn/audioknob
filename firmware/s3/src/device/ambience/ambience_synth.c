#include "ambience_synth.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Every source of variation is the PRNG -- event times, pitches, levels,
// pans, slow drifts -- so nothing has a period to give it away. Slow
// controls run once per block of CTRL_FRAMES; anything that moves within
// tens of milliseconds runs per sample.

#define CTRL_FRAMES 32
#define TWO_PI      6.28318530718f

#define RAIN_DROPS   24
#define OCEAN_WAVES  2 // a nearer and a farther line of surf
#define FOREST_BIRDS 4

// Output levels, measured off-device (see the header's -18 dBFS).
#define BROWN_GAIN 1.1f
#define PINK_GAIN  0.085f
#define RAIN_GAIN  2.0f
#define OCEAN_GAIN  3.2f
#define FOREST_GAIN 2.0f

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} biquad_t;

// A decaying sinusoid as a rotating phasor; it starts at sin(0), so its
// onset never clicks. w drifts by `chirp` per control block (a rising
// bubble).
typedef struct {
    float re, im, c, s, r, w, chirp;
    float gain_l, gain_r;
    uint32_t left;      // samples until it's inaudible
    uint32_t click;     // samples of noise tick still to add at the onset
    float click_amp;
} partial_t;

// A value that wanders between lo and hi, picking a new target every
// period_min-period_max control blocks and gliding to it.
typedef struct {
    float value, target, coef, lo, hi;
    uint32_t left, period_min, period_max;
} drift_t;

typedef struct {
    float b[7];
} pink_t;

// One breaking wave after another: a swell, the crash, then the wash
// draining back. Times are in control blocks.
typedef struct {
    uint32_t pos, period, crash; // the crash comes `crash` into `period`
    float amp, env, foam;
    float gain_l, gain_r;
    float lp1[2], lp2[2]; // two one-pole lowpasses per channel
} wave_t;

// A bird singing a phrase of similar chirps. Positions are in samples.
typedef struct {
    bool singing;
    uint32_t chirps_left, chirp_len, gap_len, pos;
    bool in_gap;
    float f_lo, f_hi;       // each chirp sweeps between these
    bool rising;
    float trill_hz, trill_depth, trill_phase;
    float amp, env_prev;
    float re, im;
    float gain_l, gain_r;
} bird_t;

struct ambience_synth {
    float fs;
    uint32_t rng;
    ambience_kind_t kind;
    uint32_t ctrl_left;

    pink_t pink[2];
    float brown[2];
    biquad_t hp[2], lp[2], room_lp[2], bus_lp[2]; // bus_lp: ocean's output highpass
    drift_t gust[2];

    partial_t parts[RAIN_DROPS];
    uint32_t next_drop, next_drip, next_song;
    drift_t intensity;

    wave_t waves[OCEAN_WAVES];
    bird_t birds[FOREST_BIRDS];
};

// -- primitives --

static inline uint32_t rnd(uint32_t *x) {
    *x ^= *x << 13;
    *x ^= *x >> 17;
    *x ^= *x << 5;
    return *x;
}

static inline float rnd_bi(uint32_t *x) {
    return (float)(int32_t)rnd(x) * (1.0f / 2147483648.0f);
}

static inline float rnd_uni(uint32_t *x) {
    return (float)(rnd(x) >> 8) * (1.0f / 16777216.0f);
}

static float rnd_range(uint32_t *x, float lo, float hi) {
    return lo + (hi - lo) * rnd_uni(x);
}

// Pitches and times are perceived on a log scale.
static float rnd_log(uint32_t *x, float lo, float hi) {
    return lo * powf(hi / lo, rnd_uni(x));
}

// Exponential gaps make a Poisson process: no rhythm to latch onto.
static uint32_t poisson_gap(ambience_synth_t *s, float rate_hz) {
    float u = rnd_uni(&s->rng);
    return (uint32_t)(-logf(1.0f - u) / rate_hz * s->fs) + 1;
}

static inline float bq_run(biquad_t *f, float x) {
    float y = f->b0 * x + f->z1;
    f->z1 = f->b1 * x - f->a1 * y + f->z2;
    f->z2 = f->b2 * x - f->a2 * y;
    return y;
}

typedef enum { BQ_LOWPASS, BQ_HIGHPASS, BQ_BANDPASS } bq_type_t;

// RBJ cookbook; the band-pass has 0 dB peak gain. Keeps the filter state,
// so a running filter can be retuned without a click.
static void bq_set(biquad_t *f, bq_type_t type, float fs, float fc, float q) {
    float w = TWO_PI * fc / fs;
    float cw = cosf(w), alpha = sinf(w) / (2.0f * q);
    float a0 = 1.0f + alpha;
    float b0, b1, b2;
    switch (type) {
    case BQ_LOWPASS:
        b0 = b2 = (1.0f - cw) / 2.0f;
        b1 = 1.0f - cw;
        break;
    case BQ_HIGHPASS:
        b0 = b2 = (1.0f + cw) / 2.0f;
        b1 = -(1.0f + cw);
        break;
    default:
        b0 = alpha;
        b1 = 0.0f;
        b2 = -alpha;
        break;
    }
    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = -2.0f * cw / a0;
    f->a2 = (1.0f - alpha) / a0;
}

// Paul Kellet's refined pink filter: within 0.05 dB of -3 dB/octave above
// 9 Hz.
static inline float pink_run(pink_t *p, float w) {
    float *b = p->b;
    b[0] = 0.99886f * b[0] + w * 0.0555179f;
    b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.96900f * b[2] + w * 0.1538520f;
    b[3] = 0.86650f * b[3] + w * 0.3104856f;
    b[4] = 0.55000f * b[4] + w * 0.5329522f;
    b[5] = -0.7616f * b[5] - w * 0.0168980f;
    float out = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
    b[6] = w * 0.115926f;
    return out;
}

// A leaky integrator, corner ~140 Hz: steeper than pink, without the
// subsonic wander of a true random walk.
static inline float brown_run(float *state, float w) {
    *state = 0.98f * *state + 0.043f * w;
    return *state;
}

static void drift_init(drift_t *d, ambience_synth_t *s, float lo, float hi, float period_min_s,
                       float period_max_s, float glide_s) {
    float blocks_per_s = s->fs / CTRL_FRAMES;
    d->lo = lo;
    d->hi = hi;
    d->period_min = (uint32_t)(period_min_s * blocks_per_s);
    d->period_max = (uint32_t)(period_max_s * blocks_per_s);
    d->coef = 1.0f - expf(-1.0f / (glide_s * blocks_per_s));
    d->value = d->target = rnd_range(&s->rng, lo, hi);
    d->left = 0;
}

static float drift_step(drift_t *d, uint32_t *rng) {
    if (d->left == 0) {
        d->target = rnd_range(rng, d->lo, d->hi);
        d->left = d->period_min + rnd(rng) % (d->period_max - d->period_min + 1);
    }
    d->left--;
    d->value += (d->target - d->value) * d->coef;
    return d->value;
}

static void pan_gains(float pan, float *l, float *r) {
    // Constant power, pan in [-1, 1].
    float a = (pan + 1.0f) * (TWO_PI / 8.0f);
    *l = cosf(a);
    *r = sinf(a);
}

// -- partials (rain drops) --

static partial_t *partial_free(ambience_synth_t *s, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!s->parts[i].left) {
            return &s->parts[i];
        }
    }
    return NULL;
}

static void partial_start(ambience_synth_t *s, partial_t *p, float freq, float decay_s, float amp, float pan,
                          float chirp) {
    p->w = TWO_PI * freq / s->fs;
    p->c = cosf(p->w);
    p->s = sinf(p->w);
    p->r = expf(-1.0f / (decay_s * s->fs));
    p->re = amp;
    p->im = 0.0f;
    p->chirp = chirp;
    // Down 60 dB.
    p->left = (uint32_t)(decay_s * 6.9f * s->fs);
    p->click = 0;
    pan_gains(pan, &p->gain_l, &p->gain_r);
}

static void partials_control(partial_t *parts, size_t count) {
    for (size_t i = 0; i < count; i++) {
        partial_t *p = &parts[i];
        if (p->left && p->chirp != 1.0f) {
            p->w *= p->chirp;
            p->c = cosf(p->w);
            p->s = sinf(p->w);
        }
    }
}

static void partials_render(ambience_synth_t *s, size_t count, float *out, size_t frames) {
    for (size_t i = 0; i < count; i++) {
        partial_t *p = &s->parts[i];
        if (!p->left) {
            continue;
        }
        size_t n = frames < p->left ? frames : p->left;
        float re = p->re, im = p->im;
        float c = p->c * p->r, sn = p->s * p->r;
        for (size_t k = 0; k < n; k++) {
            float nre = re * c - im * sn;
            im = re * sn + im * c;
            re = nre;
            float y = im;
            if (p->click) {
                p->click--;
                y += rnd_bi(&s->rng) * p->click_amp * (float)p->click * (1.0f / 48.0f);
            }
            out[2 * k] += y * p->gain_l;
            out[2 * k + 1] += y * p->gain_r;
        }
        p->re = re;
        p->im = im;
        p->left -= (uint32_t)n;
    }
}

// -- noise colors --

static void render_brown(ambience_synth_t *s, float *out, size_t frames) {
    for (size_t k = 0; k < frames; k++) {
        for (int ch = 0; ch < 2; ch++) {
            float x = brown_run(&s->brown[ch], rnd_bi(&s->rng));
            out[2 * k + ch] = bq_run(&s->hp[ch], x) * BROWN_GAIN;
        }
    }
}

static void render_pink(ambience_synth_t *s, float *out, size_t frames) {
    for (size_t k = 0; k < frames; k++) {
        for (int ch = 0; ch < 2; ch++) {
            float x = pink_run(&s->pink[ch], rnd_bi(&s->rng));
            out[2 * k + ch] = bq_run(&s->hp[ch], x) * PINK_GAIN;
        }
    }
}

// -- rain --

#define RAIN_DROP_HZ 55.0f // light-to-moderate rain on something nearby
#define RAIN_DRIP_HZ 0.5f  // the occasional big drop off an eave

static void rain_reset(ambience_synth_t *s) {
    for (int ch = 0; ch < 2; ch++) {
        bq_set(&s->hp[ch], BQ_HIGHPASS, s->fs, 350.0f, 0.707f);
        bq_set(&s->lp[ch], BQ_LOWPASS, s->fs, 7500.0f, 0.707f);
        bq_set(&s->room_lp[ch], BQ_LOWPASS, s->fs, 220.0f, 0.707f);
        drift_init(&s->gust[ch], s, 0.75f, 1.0f, 2.0f, 7.0f, 1.5f);
    }
    drift_init(&s->intensity, s, 0.6f, 1.0f, 8.0f, 25.0f, 4.0f);
    s->next_drop = poisson_gap(s, RAIN_DROP_HZ);
    s->next_drip = poisson_gap(s, RAIN_DRIP_HZ);
}

static void rain_drop(ambience_synth_t *s, bool drip) {
    partial_t *p = partial_free(s, RAIN_DROPS);
    if (!p) {
        return;
    }
    float u = rnd_uni(&s->rng);
    float pan = rnd_bi(&s->rng) * 0.8f;
    if (drip) {
        // A bigger drop rings lower and rises more as its bubble shrinks.
        partial_start(s, p, rnd_log(&s->rng, 700.0f, 1500.0f), rnd_range(&s->rng, 0.012f, 0.03f),
                      0.05f + 0.07f * u, pan, 1.0f + rnd_range(&s->rng, 0.004f, 0.012f));
    } else {
        // Mostly faint ticks, now and then a clear one.
        partial_start(s, p, rnd_log(&s->rng, 1800.0f, 6000.0f), rnd_range(&s->rng, 0.002f, 0.007f),
                      0.01f + 0.09f * u * u * u, pan, 1.0f + rnd_range(&s->rng, 0.0f, 0.006f));
    }
    p->click = 48;
    p->click_amp = p->re * 0.6f;
}

static void rain_control(ambience_synth_t *s, float *gust, float *intensity) {
    *intensity = drift_step(&s->intensity, &s->rng);
    gust[0] = drift_step(&s->gust[0], &s->rng);
    gust[1] = drift_step(&s->gust[1], &s->rng);
    partials_control(s->parts, RAIN_DROPS);
}

static void render_rain(ambience_synth_t *s, float *out, size_t frames) {
    float gust[2], intensity;
    rain_control(s, gust, &intensity);
    for (size_t k = 0; k < frames; k++) {
        for (int ch = 0; ch < 2; ch++) {
            float w = rnd_bi(&s->rng);
            float hiss = bq_run(&s->lp[ch], bq_run(&s->hp[ch], pink_run(&s->pink[ch], w)));
            float rumble = bq_run(&s->room_lp[ch], brown_run(&s->brown[ch], w));
            out[2 * k + ch] = (hiss * 0.075f * gust[ch] + rumble * 0.35f) * intensity * RAIN_GAIN;
        }
        if (--s->next_drop == 0) {
            rain_drop(s, false);
            s->next_drop = poisson_gap(s, RAIN_DROP_HZ * intensity);
        }
        if (--s->next_drip == 0) {
            rain_drop(s, true);
            s->next_drip = poisson_gap(s, RAIN_DRIP_HZ);
        }
    }
    partials_render(s, RAIN_DROPS, out, frames);
}

// -- ocean --

static void wave_next(ambience_synth_t *s, wave_t *w, bool far) {
    float blocks_per_s = s->fs / CTRL_FRAMES;
    w->period = (uint32_t)(rnd_range(&s->rng, 6.0f, 13.0f) * blocks_per_s);
    w->crash = (uint32_t)(w->period * rnd_range(&s->rng, 0.35f, 0.5f));
    w->pos = 0;
    w->amp = far ? rnd_range(&s->rng, 0.25f, 0.5f) : rnd_range(&s->rng, 0.55f, 1.0f);
    pan_gains(rnd_bi(&s->rng) * (far ? 0.6f : 0.35f), &w->gain_l, &w->gain_r);
}

static void ocean_reset(ambience_synth_t *s) {
    for (int ch = 0; ch < 2; ch++) {
        bq_set(&s->room_lp[ch], BQ_LOWPASS, s->fs, 150.0f, 0.707f);
        bq_set(&s->hp[ch], BQ_HIGHPASS, s->fs, 2500.0f, 0.707f);
        // The lowest octave is mostly headroom a desk speaker can't use.
        bq_set(&s->bus_lp[ch], BQ_HIGHPASS, s->fs, 70.0f, 0.707f);
    }
    for (int i = 0; i < OCEAN_WAVES; i++) {
        wave_next(s, &s->waves[i], i == 1);
        // Start mid-cycle, and the two out of step.
        s->waves[i].pos = rnd(&s->rng) % s->waves[i].period;
    }
}

// The swell builds slowly to the crash, then the wash decays; foam (the
// hiss of water running back over sand) trails it.
static void wave_control(ambience_synth_t *s, wave_t *w, bool far) {
    if (++w->pos >= w->period) {
        wave_next(s, w, far);
    }
    float env;
    if (w->pos < w->crash) {
        float t = (float)w->pos / (float)w->crash;
        env = t * t * t;
    } else {
        float t = (float)(w->pos - w->crash) / (float)(w->period - w->crash);
        env = expf(-3.5f * t) * (1.0f - t * t);
    }
    w->env = env * w->amp;
    float foam_target = w->pos >= w->crash ? w->env : 0.0f;
    w->foam += (foam_target - w->foam) * 0.004f;
}

static void render_ocean(ambience_synth_t *s, float *out, size_t frames) {
    float k[OCEAN_WAVES], g[OCEAN_WAVES];
    for (int i = 0; i < OCEAN_WAVES; i++) {
        wave_t *w = &s->waves[i];
        wave_control(s, w, i == 1);
        // A crash is bright, the swell and a far wave are dull.
        float fc = (i == 1 ? 250.0f : 350.0f) + (i == 1 ? 1200.0f : 3000.0f) * w->env;
        float c = TWO_PI * fc / s->fs;
        k[i] = c > 1.0f ? 1.0f : c;
        g[i] = w->env;
    }
    for (size_t n = 0; n < frames; n++) {
        for (int ch = 0; ch < 2; ch++) {
            float w = rnd_bi(&s->rng);
            float x = pink_run(&s->pink[ch], w) * PINK_GAIN;
            float y = bq_run(&s->room_lp[ch], brown_run(&s->brown[ch], w)) * 0.05f;
            for (int i = 0; i < OCEAN_WAVES; i++) {
                wave_t *w = &s->waves[i];
                w->lp1[ch] += (x - w->lp1[ch]) * k[i];
                w->lp2[ch] += (w->lp1[ch] - w->lp2[ch]) * k[i];
                float gain = ch ? w->gain_r : w->gain_l;
                y += w->lp2[ch] * g[i] * gain;
            }
            float foam = bq_run(&s->hp[ch], x) * (s->waves[0].foam + 0.5f * s->waves[1].foam);
            out[2 * n + ch] = bq_run(&s->bus_lp[ch], y + foam * 0.5f) * OCEAN_GAIN;
        }
    }
}

// -- forest --

#define FOREST_SONG_HZ 0.35f

static void forest_reset(ambience_synth_t *s) {
    for (int ch = 0; ch < 2; ch++) {
        bq_set(&s->hp[ch], BQ_HIGHPASS, s->fs, 400.0f, 0.707f);
        bq_set(&s->lp[ch], BQ_LOWPASS, s->fs, 3500.0f, 0.707f);
        drift_init(&s->gust[ch], s, 0.25f, 1.0f, 1.5f, 6.0f, 1.2f);
    }
    drift_init(&s->intensity, s, 0.5f, 1.0f, 6.0f, 20.0f, 3.0f);
    s->next_song = poisson_gap(s, 1.0f);
}

// A new song from whoever is free: a pitch range, a contour and a pace
// that its chirps share, somewhere in the trees.
static void bird_sing(ambience_synth_t *s) {
    bird_t *b = NULL;
    for (int i = 0; i < FOREST_BIRDS; i++) {
        if (!s->birds[i].singing) {
            b = &s->birds[i];
            break;
        }
    }
    if (!b) {
        return;
    }
    float f = rnd_log(&s->rng, 2000.0f, 6000.0f);
    b->f_lo = f;
    b->f_hi = f * rnd_range(&s->rng, 1.05f, 1.6f);
    b->rising = rnd_uni(&s->rng) < 0.5f;
    b->chirps_left = 1 + rnd(&s->rng) % 7;
    b->chirp_len = (uint32_t)(rnd_range(&s->rng, 0.035f, 0.16f) * s->fs);
    b->gap_len = (uint32_t)(rnd_range(&s->rng, 0.04f, 0.2f) * s->fs);
    // Some warble.
    bool trill = rnd_uni(&s->rng) < 0.4f;
    b->trill_hz = rnd_range(&s->rng, 18.0f, 45.0f);
    b->trill_depth = trill ? rnd_range(&s->rng, 0.03f, 0.1f) : 0.0f;
    b->trill_phase = 0.0f;
    b->amp = rnd_range(&s->rng, 0.015f, 0.09f);
    pan_gains(rnd_bi(&s->rng) * 0.9f, &b->gain_l, &b->gain_r);
    b->singing = true;
    b->in_gap = false;
    b->pos = 0;
    b->re = 1.0f;
    b->im = 0.0f;
    b->env_prev = 0.0f;
}

// A control block of one bird: the pitch is held for the block, but the
// phasor keeps the phase continuous across steps.
static void bird_render(ambience_synth_t *s, bird_t *b, float *out, size_t frames) {
    uint32_t len = b->in_gap ? b->gap_len : b->chirp_len;
    if (b->in_gap) {
        b->pos += (uint32_t)frames;
        if (b->pos >= len) {
            b->in_gap = false;
            b->pos = 0;
        }
        return;
    }
    float t = (float)b->pos / (float)len;
    float sweep = b->rising ? t : 1.0f - t;
    b->trill_phase += TWO_PI * b->trill_hz * (float)frames / s->fs;
    float f = (b->f_lo + (b->f_hi - b->f_lo) * sweep) * (1.0f + b->trill_depth * sinf(b->trill_phase));
    float w = TWO_PI * f / s->fs;
    float c = cosf(w), sn = sinf(w);
    float t_end = (float)(b->pos + frames) / (float)len;
    t_end = t_end > 1.0f ? 1.0f : t_end;
    // A smooth bump per chirp, ramped linearly across the block.
    float env_end = 4.0f * t_end * (1.0f - t_end);
    float env = b->env_prev, step = (env_end - env) / (float)frames;
    float re = b->re, im = b->im;
    for (size_t k = 0; k < frames; k++) {
        float nre = re * c - im * sn;
        im = re * sn + im * c;
        re = nre;
        float y = im * env * b->amp;
        env += step;
        out[2 * k] += y * b->gain_l;
        out[2 * k + 1] += y * b->gain_r;
    }
    // Rotation drifts the magnitude a little each block.
    float mag = sqrtf(re * re + im * im);
    b->re = re / mag;
    b->im = im / mag;
    b->env_prev = env_end;
    b->pos += (uint32_t)frames;
    if (b->pos >= len) {
        b->pos = 0;
        b->env_prev = 0.0f;
        b->in_gap = true;
        if (--b->chirps_left == 0) {
            b->singing = false;
        }
    }
}

static void render_forest(ambience_synth_t *s, float *out, size_t frames) {
    float intensity = drift_step(&s->intensity, &s->rng);
    float gust[2] = {drift_step(&s->gust[0], &s->rng), drift_step(&s->gust[1], &s->rng)};
    // Leaves: wind noise, its level gusting independently either side.
    for (size_t k = 0; k < frames; k++) {
        for (int ch = 0; ch < 2; ch++) {
            float x = pink_run(&s->pink[ch], rnd_bi(&s->rng));
            float leaves = bq_run(&s->lp[ch], bq_run(&s->hp[ch], x));
            out[2 * k + ch] = leaves * 0.09f * gust[ch] * intensity * FOREST_GAIN;
        }
    }
    if (s->next_song <= frames) {
        bird_sing(s);
        s->next_song = poisson_gap(s, FOREST_SONG_HZ);
    } else {
        s->next_song -= (uint32_t)frames;
    }
    for (int i = 0; i < FOREST_BIRDS; i++) {
        if (s->birds[i].singing) {
            bird_render(s, &s->birds[i], out, frames);
        }
    }
}

// -- public --

ambience_synth_t *ambience_synth_create(uint32_t sample_rate, uint32_t seed) {
    ambience_synth_t *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->fs = (float)sample_rate;
    s->rng = seed ? seed : 0x9e3779b9u;
    ambience_synth_reset(s, AMBIENCE_BROWN);
    return s;
}

void ambience_synth_reset(ambience_synth_t *s, ambience_kind_t kind) {
    uint32_t rng = s->rng;
    float fs = s->fs;
    memset(s, 0, sizeof(*s));
    s->rng = rng;
    s->fs = fs;
    s->kind = kind;
    switch (kind) {
    case AMBIENCE_BROWN:
    case AMBIENCE_PINK:
        // Below what any desk speaker plays; only costs headroom.
        for (int ch = 0; ch < 2; ch++) {
            bq_set(&s->hp[ch], BQ_HIGHPASS, fs, 30.0f, 0.707f);
        }
        break;
    case AMBIENCE_RAIN:
        rain_reset(s);
        break;
    case AMBIENCE_OCEAN:
        ocean_reset(s);
        break;
    case AMBIENCE_FOREST:
        forest_reset(s);
        break;
    default:
        break;
    }
}

void ambience_synth_render(ambience_synth_t *s, float *out, size_t frames) {
    while (frames > 0) {
        size_t n = frames < CTRL_FRAMES ? frames : CTRL_FRAMES;
        switch (s->kind) {
        case AMBIENCE_BROWN:
            render_brown(s, out, n);
            break;
        case AMBIENCE_PINK:
            render_pink(s, out, n);
            break;
        case AMBIENCE_RAIN:
            render_rain(s, out, n);
            break;
        case AMBIENCE_OCEAN:
            render_ocean(s, out, n);
            break;
        case AMBIENCE_FOREST:
            render_forest(s, out, n);
            break;
        default:
            memset(out, 0, n * 2 * sizeof(float));
            break;
        }
        out += 2 * n;
        frames -= n;
    }
}
