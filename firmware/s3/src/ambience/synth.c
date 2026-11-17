#include "ambience/synth.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Every source of variation is the PRNG -- event times, pitches, levels,
// pans, slow drifts -- so nothing has a period to give it away. Slow
// controls run once per block of CTRL_FRAMES; anything that moves within
// tens of milliseconds runs per sample.
//
// Most natural textures here are many small events rather than a filtered
// hiss: a drop hitting a leaf, a bubble ringing, an ember cracking. Two
// event voices cover them: ticks (noise bursts, so many that they share one
// decaying envelope per channel) and partials (a bubble's decaying, rising
// tone).

#define CTRL_FRAMES 32
#define TWO_PI      6.28318530718f

#define PARTIALS     24
#define OCEAN_WAVES  2 // a nearer and a farther line of surf
#define FOREST_BIRDS 5

// Kellet's pink filter has a large gain; this brings it near the others'.
#define PINK_NORM 0.085f

// Output levels, measured off-device (see the header's -18 dBFS).
#define RAIN_GAIN      2.7f
#define STORM_GAIN     1.3f
#define OCEAN_GAIN     3.9f
#define FOREST_GAIN    4.6f
#define STREAM_GAIN    4.6f
#define FIREPLACE_GAIN 2.5f
#define WIND_GAIN      3.4f

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} biquad_t;

// A decaying sinusoid as a rotating phasor; it starts at sin(0), so its
// onset never clicks. w scales by `chirp` per control block (a rising
// bubble).
typedef struct {
    float re, im, c, s, r, w, chirp;
    float gain_l, gain_r;
    uint32_t left; // samples until it's inaudible
} partial_t;

// Noise bursts too short and many to voice one by one: each hit adds to a
// per-channel envelope that decays every sample, and the noise through it
// is one draw per sample.
typedef struct {
    float a[2], decay;
    uint32_t next; // samples to the next hit
} ticks_t;

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

// A clap, then rumble rolling on for seconds, duller as it goes. Times in
// control blocks.
typedef struct {
    bool active;
    uint32_t pos, len, attack;
    float amp, crack, fc_start, fc_end;
    float gain_l, gain_r;
    drift_t roll;
    biquad_t lp[2];
    float brown[2];
} thunder_t;

typedef enum { SYL_WHISTLE, SYL_DOWN, SYL_UP, SYL_CHEVRON, SYL_TRILL, SYL_COUNT } syllable_t;

// A resident bird: its species (pitch, note shape, pace, distance) is fixed
// for the session, so the same songs come back from the same place, as in
// a real wood. Positions are in samples.
typedef struct {
    float f0, span, step, harm, amp, lp_k, gain_l, gain_r, trill_hz;
    syllable_t syl, last_syl;
    uint32_t note_len, gap_len, song_gap_s_min, song_gap_s_max;
    uint8_t notes_min, notes_max;

    bool singing, in_gap;
    uint32_t notes_left, pos, until_song;
    float pitch, phase, env_prev, re, im, lp;
} bird_t;

struct ambience_synth {
    float fs;
    uint32_t rng;
    ambience_kind_t kind;

    pink_t pink[2];
    float brown[2];
    biquad_t wash_hp[2], wash_lp[2], low_lp[2], bus_hp[2], tick_f[2], thump_f[2];
    drift_t gust[2];
    drift_t intensity, flicker;

    ticks_t ticks, thumps;
    float tick_rate_hz;

    partial_t parts[PARTIALS];
    uint32_t next_bubble, next_drip;

    wave_t waves[OCEAN_WAVES];
    thunder_t thunder;
    uint32_t next_thunder; // control blocks
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

static float blocks_per_s(const ambience_synth_t *s) {
    return s->fs / CTRL_FRAMES;
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

static void bq_set_both(biquad_t f[2], bq_type_t type, float fs, float fc, float q) {
    bq_set(&f[0], type, fs, fc, q);
    bq_set(&f[1], type, fs, fc, q);
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

// Unity below 0.5, then bending smoothly toward 1.0: rare peaks (a crash,
// a pop, a clap of thunder) round off instead of clipping.
static inline float soft_clip(float x) {
    float a = fabsf(x);
    if (a <= 0.5f) {
        return x;
    }
    float over = a - 0.5f;
    return copysignf(0.5f + over / (1.0f + 2.0f * over), x);
}

static void drift_init(drift_t *d, ambience_synth_t *s, float lo, float hi, float period_min_s,
                       float period_max_s, float glide_s) {
    float bps = blocks_per_s(s);
    d->lo = lo;
    d->hi = hi;
    d->period_min = (uint32_t)(period_min_s * bps) + 1;
    d->period_max = (uint32_t)(period_max_s * bps) + 1;
    d->coef = 1.0f - expf(-1.0f / (glide_s * bps));
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

// -- ticks --

static void ticks_init(ticks_t *t, ambience_synth_t *s, float decay_s) {
    t->a[0] = t->a[1] = 0.0f;
    t->decay = expf(-1.0f / (decay_s * s->fs));
    t->next = 1;
}

static void ticks_hit(ticks_t *t, uint32_t *rng, float amp, float spread) {
    float l, r;
    pan_gains(rnd_bi(rng) * spread, &l, &r);
    t->a[0] += amp * l;
    t->a[1] += amp * r;
}

// This sample of channel `ch`; ticks_advance() once both are read.
static inline float ticks_out(const ticks_t *t, int ch, uint32_t *rng) {
    return rnd_bi(rng) * t->a[ch];
}

static inline void ticks_advance(ticks_t *t) {
    t->a[0] *= t->decay;
    t->a[1] *= t->decay;
}

// -- partials (bubbles, drops) --

static partial_t *partial_free(ambience_synth_t *s) {
    for (size_t i = 0; i < PARTIALS; i++) {
        if (!s->parts[i].left) {
            return &s->parts[i];
        }
    }
    return NULL;
}

// `rise` is how far the pitch climbs over the partial's life (1: steady).
static void partial_start(ambience_synth_t *s, float freq, float decay_s, float amp, float pan, float rise) {
    partial_t *p = partial_free(s);
    if (!p) {
        return;
    }
    p->w = TWO_PI * freq / s->fs;
    p->c = cosf(p->w);
    p->s = sinf(p->w);
    p->r = expf(-1.0f / (decay_s * s->fs));
    p->re = amp;
    p->im = 0.0f;
    // Down 40 dB: under the wash it's long gone.
    p->left = (uint32_t)(decay_s * 4.6f * s->fs) + 1;
    p->chirp = rise == 1.0f ? 1.0f : powf(rise, (float)CTRL_FRAMES / (float)p->left);
    pan_gains(pan, &p->gain_l, &p->gain_r);
}

static void partials_control(partial_t *parts) {
    for (size_t i = 0; i < PARTIALS; i++) {
        partial_t *p = &parts[i];
        if (p->left && p->chirp != 1.0f) {
            p->w *= p->chirp;
            p->c = cosf(p->w);
            p->s = sinf(p->w);
        }
    }
}

static void partials_render(ambience_synth_t *s, float *out, size_t frames) {
    partials_control(s->parts);
    for (size_t i = 0; i < PARTIALS; i++) {
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
            out[2 * k] += im * p->gain_l;
            out[2 * k + 1] += im * p->gain_r;
        }
        p->re = re;
        p->im = im;
        p->left -= (uint32_t)n;
    }
}

// -- rain and storm --

// Rain is mostly the patter of drops striking leaves and ground (ticks);
// only some trap a bubble that rings (partials), and now and then a big
// drop falls off an eave.
#define RAIN_PATTER_HZ  420.0f
#define RAIN_BUBBLE_HZ  9.0f
#define RAIN_DRIP_HZ    0.4f
#define STORM_THUNDER_S 40.0f // mean gap between claps

static void rain_reset(ambience_synth_t *s, bool storm) {
    float fs = s->fs;
    bq_set_both(s->wash_hp, BQ_HIGHPASS, fs, storm ? 250.0f : 400.0f, 0.707f);
    bq_set_both(s->wash_lp, BQ_LOWPASS, fs, storm ? 9000.0f : 6500.0f, 0.707f);
    bq_set_both(s->low_lp, BQ_LOWPASS, fs, storm ? 300.0f : 220.0f, 0.707f);
    bq_set_both(s->tick_f, BQ_HIGHPASS, fs, 1200.0f, 0.707f);
    bq_set_both(s->bus_hp, BQ_HIGHPASS, fs, 35.0f, 0.707f);
    for (int ch = 0; ch < 2; ch++) {
        drift_init(&s->gust[ch], s, 0.75f, 1.0f, 2.0f, 7.0f, 1.5f);
    }
    if (storm) {
        drift_init(&s->intensity, s, 1.0f, 1.5f, 6.0f, 20.0f, 3.0f);
        s->next_thunder = (uint32_t)(rnd_range(&s->rng, 6.0f, 18.0f) * blocks_per_s(s));
    } else {
        drift_init(&s->intensity, s, 0.6f, 1.0f, 8.0f, 25.0f, 4.0f);
    }
    ticks_init(&s->ticks, s, 0.0006f);
    s->next_bubble = poisson_gap(s, RAIN_BUBBLE_HZ);
    s->next_drip = poisson_gap(s, RAIN_DRIP_HZ);
}

static void rain_events(ambience_synth_t *s, float intensity) {
    if (--s->ticks.next == 0) {
        // Mostly faint, a few sharp: the nearest drops stand out.
        float u = rnd_uni(&s->rng);
        ticks_hit(&s->ticks, &s->rng, 0.015f + 0.22f * u * u * u * u, 0.9f);
        s->ticks.next = poisson_gap(s, RAIN_PATTER_HZ * intensity * intensity);
    }
    if (--s->next_bubble == 0) {
        float u = rnd_uni(&s->rng);
        partial_start(s, rnd_log(&s->rng, 1500.0f, 4500.0f), rnd_range(&s->rng, 0.002f, 0.006f),
                      0.008f + 0.05f * u * u * u, rnd_bi(&s->rng) * 0.8f, rnd_range(&s->rng, 1.05f, 1.3f));
        s->next_bubble = poisson_gap(s, RAIN_BUBBLE_HZ * intensity);
    }
    if (--s->next_drip == 0) {
        // A bigger drop rings lower and rises more as its bubble shrinks.
        partial_start(s, rnd_log(&s->rng, 700.0f, 1500.0f), rnd_range(&s->rng, 0.012f, 0.03f),
                      rnd_range(&s->rng, 0.03f, 0.09f), rnd_bi(&s->rng) * 0.8f, rnd_range(&s->rng, 1.2f, 1.6f));
        s->next_drip = poisson_gap(s, RAIN_DRIP_HZ);
    }
}

static void thunder_start(ambience_synth_t *s) {
    thunder_t *t = &s->thunder;
    float bps = blocks_per_s(s);
    float near = rnd_uni(&s->rng); // 1: right overhead
    memset(t, 0, sizeof(*t));
    t->active = true;
    t->len = (uint32_t)(rnd_range(&s->rng, 5.0f, 12.0f) * bps);
    // Far thunder swells in; near thunder hits at once.
    t->attack = (uint32_t)(rnd_range(&s->rng, 0.05f, 0.25f + 1.5f * (1.0f - near)) * bps) + 1;
    t->amp = 0.35f + 0.65f * near;
    t->crack = near > 0.7f ? (near - 0.7f) * 2.0f : 0.0f;
    t->fc_start = 250.0f + 900.0f * near;
    t->fc_end = rnd_range(&s->rng, 70.0f, 110.0f);
    pan_gains(rnd_bi(&s->rng) * 0.6f, &t->gain_l, &t->gain_r);
    drift_init(&t->roll, s, 0.15f, 1.0f, 0.08f, 0.6f, 0.07f);
    bq_set_both(t->lp, BQ_LOWPASS, s->fs, t->fc_start, 0.707f);
}

// Envelope and tone for this block; 0 once it's over.
static float thunder_control(ambience_synth_t *s) {
    thunder_t *t = &s->thunder;
    if (!t->active) {
        if (s->next_thunder-- == 0) {
            thunder_start(s);
            s->next_thunder = poisson_gap(s, 1.0f / STORM_THUNDER_S) / CTRL_FRAMES;
        }
        return 0.0f;
    }
    if (++t->pos >= t->len) {
        t->active = false;
        return 0.0f;
    }
    float env;
    if (t->pos < t->attack) {
        float a = (float)t->pos / (float)t->attack;
        env = a * a;
    } else {
        float d = (float)(t->pos - t->attack) / (float)(t->len - t->attack);
        env = expf(-3.0f * d) * (1.0f - d);
    }
    float d = (float)t->pos / (float)t->len;
    if (t->pos % 8 == 0) {
        float fc = t->fc_start * powf(t->fc_end / t->fc_start, sqrtf(d));
        bq_set_both(t->lp, BQ_LOWPASS, s->fs, fc, 0.8f);
    }
    return env * t->amp * drift_step(&t->roll, &s->rng);
}

static void render_rain(ambience_synth_t *s, float *out, size_t frames) {
    bool storm = s->kind == AMBIENCE_STORM;
    float intensity = drift_step(&s->intensity, &s->rng);
    float gust[2] = {drift_step(&s->gust[0], &s->rng), drift_step(&s->gust[1], &s->rng)};
    float thunder = storm ? thunder_control(s) : 0.0f;
    thunder_t *t = &s->thunder;
    float crack_decay = expf(-1.0f / (0.12f * s->fs));
    float wash = (storm ? 0.065f : 0.05f) * intensity;
    float rumble = (storm ? 0.45f : 0.3f) * intensity;
    for (size_t k = 0; k < frames; k++) {
        rain_events(s, intensity);
        for (int ch = 0; ch < 2; ch++) {
            float w = rnd_bi(&s->rng);
            float hiss = bq_run(&s->wash_lp[ch], bq_run(&s->wash_hp[ch], pink_run(&s->pink[ch], w)));
            float low = bq_run(&s->low_lp[ch], brown_run(&s->brown[ch], w));
            float patter = bq_run(&s->tick_f[ch], ticks_out(&s->ticks, ch, &s->rng));
            float y = hiss * wash * gust[ch] + low * rumble + patter;
            if (thunder > 0.0f) {
                float tw = rnd_bi(&s->rng);
                float boom = bq_run(&t->lp[ch], brown_run(&t->brown[ch], tw)) * 12.0f;
                float crack = tw * t->crack * 0.25f;
                y += (boom * thunder + crack) * (ch ? t->gain_r : t->gain_l);
            }
            out[2 * k + ch] = y;
        }
        ticks_advance(&s->ticks);
        t->crack *= crack_decay;
    }
    partials_render(s, out, frames);
    float gain = storm ? STORM_GAIN : RAIN_GAIN;
    for (size_t k = 0; k < frames; k++) {
        for (int ch = 0; ch < 2; ch++) {
            out[2 * k + ch] = soft_clip(bq_run(&s->bus_hp[ch], out[2 * k + ch]) * gain);
        }
    }
}

// -- ocean --

#define OCEAN_FIZZ_HZ 900.0f // bubbles bursting in the foam at its thickest

static void wave_next(ambience_synth_t *s, wave_t *w, bool far) {
    float bps = blocks_per_s(s);
    w->period = (uint32_t)(rnd_range(&s->rng, 6.0f, 13.0f) * bps);
    w->crash = (uint32_t)(w->period * rnd_range(&s->rng, 0.35f, 0.5f));
    w->pos = 0;
    w->amp = far ? rnd_range(&s->rng, 0.25f, 0.5f) : rnd_range(&s->rng, 0.55f, 1.0f);
    pan_gains(rnd_bi(&s->rng) * (far ? 0.6f : 0.35f), &w->gain_l, &w->gain_r);
}

static void ocean_reset(ambience_synth_t *s) {
    float fs = s->fs;
    bq_set_both(s->low_lp, BQ_LOWPASS, fs, 140.0f, 0.707f);
    bq_set_both(s->wash_hp, BQ_HIGHPASS, fs, 2500.0f, 0.707f);
    bq_set_both(s->tick_f, BQ_HIGHPASS, fs, 3000.0f, 0.707f);
    // The lowest octave is mostly headroom a desk speaker can't use.
    bq_set_both(s->bus_hp, BQ_HIGHPASS, fs, 60.0f, 0.707f);
    ticks_init(&s->ticks, s, 0.0003f);
    for (int i = 0; i < OCEAN_WAVES; i++) {
        wave_next(s, &s->waves[i], i == 1);
        // Start mid-cycle, and the two out of step.
        s->waves[i].pos = rnd(&s->rng) % s->waves[i].period;
    }
}

// The swell builds slowly to the crash, then the wash decays; foam (water
// running back over sand, fizzing) trails it.
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
    float swell = 0.0f;
    for (int i = 0; i < OCEAN_WAVES; i++) {
        wave_t *w = &s->waves[i];
        wave_control(s, w, i == 1);
        // A crash is bright, the swell and a far wave are dull.
        float fc = (i == 1 ? 250.0f : 350.0f) + (i == 1 ? 1200.0f : 3000.0f) * w->env;
        float c = TWO_PI * fc / s->fs;
        k[i] = c > 1.0f ? 1.0f : c;
        g[i] = w->env;
        swell += w->env;
    }
    float foam = s->waves[0].foam + 0.5f * s->waves[1].foam;
    float body = 0.04f + 0.18f * swell;
    for (size_t n = 0; n < frames; n++) {
        if (--s->ticks.next == 0) {
            float u = rnd_uni(&s->rng);
            ticks_hit(&s->ticks, &s->rng, 0.02f + 0.12f * u * u * u, 0.8f);
            s->ticks.next = foam > 0.02f ? poisson_gap(s, OCEAN_FIZZ_HZ * foam) : (uint32_t)(s->fs / 50.0f);
        }
        for (int ch = 0; ch < 2; ch++) {
            float w = rnd_bi(&s->rng);
            float x = pink_run(&s->pink[ch], w) * PINK_NORM;
            float y = bq_run(&s->low_lp[ch], brown_run(&s->brown[ch], w)) * body;
            for (int i = 0; i < OCEAN_WAVES; i++) {
                wave_t *wv = &s->waves[i];
                wv->lp1[ch] += (x - wv->lp1[ch]) * k[i];
                wv->lp2[ch] += (wv->lp1[ch] - wv->lp2[ch]) * k[i];
                y += wv->lp2[ch] * g[i] * (ch ? wv->gain_r : wv->gain_l);
            }
            float hiss = bq_run(&s->wash_hp[ch], x) * foam * 0.3f;
            float fizz = bq_run(&s->tick_f[ch], ticks_out(&s->ticks, ch, &s->rng)) * foam;
            out[2 * n + ch] = soft_clip(bq_run(&s->bus_hp[ch], y + hiss + fizz) * OCEAN_GAIN);
        }
        ticks_advance(&s->ticks);
    }
}

// -- forest --

// The breeze stays well under the birds: a soft, dull rustle that all but
// stops in the lulls.
#define FOREST_RUSTLE_HZ 50.0f

static void bird_species(ambience_synth_t *s, bird_t *b) {
    uint32_t *r = &s->rng;
    memset(b, 0, sizeof(*b));
    b->f0 = rnd_log(r, 1700.0f, 5000.0f);
    b->span = rnd_range(r, 1.15f, 1.8f);
    b->syl = (syllable_t)(rnd(r) % SYL_COUNT);
    // Many songs end on a different flourish.
    b->last_syl = rnd_uni(r) < 0.4f ? (syllable_t)(rnd(r) % SYL_COUNT) : b->syl;
    float note_s = b->syl == SYL_TRILL ? rnd_range(r, 0.15f, 0.4f) : rnd_range(r, 0.05f, 0.2f);
    b->note_len = (uint32_t)(note_s * s->fs);
    b->gap_len = (uint32_t)(rnd_range(r, 0.03f, 0.15f) * s->fs);
    b->notes_min = (uint8_t)(1 + rnd(r) % 3);
    b->notes_max = (uint8_t)(b->notes_min + rnd(r) % 5);
    // Notes stepping down a scale, or up, or level.
    b->step = rnd_range(r, 0.94f, 1.03f);
    b->harm = rnd_range(r, 0.0f, 0.25f);
    b->trill_hz = rnd_range(r, 22.0f, 40.0f);
    b->song_gap_s_min = (uint32_t)rnd_range(r, 3.0f, 8.0f);
    b->song_gap_s_max = b->song_gap_s_min + (uint32_t)rnd_range(r, 4.0f, 14.0f);
    // A farther bird is quieter and duller.
    float far = rnd_uni(r);
    b->amp = 0.11f * (1.0f - 0.7f * far);
    float fc = 9000.0f - 6000.0f * far;
    b->lp_k = 1.0f - expf(-TWO_PI * fc / s->fs);
    pan_gains(rnd_bi(r) * 0.85f, &b->gain_l, &b->gain_r);
    b->until_song = (uint32_t)(rnd_range(r, 0.5f, 8.0f) * s->fs);
}

static void forest_reset(ambience_synth_t *s) {
    float fs = s->fs;
    bq_set_both(s->wash_hp, BQ_HIGHPASS, fs, 350.0f, 0.707f);
    bq_set_both(s->wash_lp, BQ_LOWPASS, fs, 1800.0f, 0.707f);
    bq_set_both(s->low_lp, BQ_LOWPASS, fs, 250.0f, 0.707f);
    bq_set_both(s->tick_f, BQ_BANDPASS, fs, 2500.0f, 0.8f);
    for (int ch = 0; ch < 2; ch++) {
        drift_init(&s->gust[ch], s, 0.6f, 1.0f, 1.5f, 5.0f, 1.0f);
    }
    // Lulls and gusts, shared by both sides.
    drift_init(&s->intensity, s, 0.05f, 1.0f, 4.0f, 14.0f, 3.0f);
    ticks_init(&s->ticks, s, 0.003f);
    for (int i = 0; i < FOREST_BIRDS; i++) {
        bird_species(s, &s->birds[i]);
    }
}

static void bird_song_start(ambience_synth_t *s, bird_t *b) {
    b->singing = true;
    b->in_gap = false;
    b->notes_left = b->notes_min + rnd(&s->rng) % (b->notes_max - b->notes_min + 1);
    b->pos = 0;
    // Never sung quite the same way twice.
    b->pitch = rnd_range(&s->rng, 0.97f, 1.03f);
    b->phase = 0.0f;
    b->env_prev = 0.0f;
    b->re = 1.0f;
    b->im = 0.0f;
}

// Pitch at t in [0, 1] through a note.
static float bird_freq(const bird_t *b, syllable_t syl, float t) {
    float lo = b->f0 * b->pitch;
    switch (syl) {
    case SYL_DOWN:
        return lo * powf(b->span, 1.0f - t);
    case SYL_UP:
        return lo * powf(b->span, t);
    case SYL_CHEVRON:
        return lo * powf(b->span, sinf(0.5f * TWO_PI * t));
    case SYL_TRILL:
        return lo * sqrtf(b->span) * (1.0f + 0.1f * sinf(b->phase));
    default:
        return lo * sqrtf(b->span) * (1.0f - 0.04f * t) * (1.0f + 0.008f * sinf(b->phase));
    }
}

// Quick onset, slower release, smooth both ways.
static float note_env(float t) {
    float x = t < 0.15f ? t / 0.15f : (1.0f - t) / 0.85f;
    return x * x * (3.0f - 2.0f * x);
}

// A control block of one bird: the pitch is held for the block, but the
// phasor keeps the phase continuous across steps.
static void bird_render(ambience_synth_t *s, bird_t *b, float *out, size_t frames) {
    if (!b->singing) {
        if (b->until_song <= frames) {
            bird_song_start(s, b);
        } else {
            b->until_song -= (uint32_t)frames;
        }
        return;
    }
    uint32_t len = b->in_gap ? b->gap_len : b->note_len;
    if (b->in_gap) {
        b->pos += (uint32_t)frames;
        if (b->pos >= len) {
            b->in_gap = false;
            b->pos = 0;
            b->pitch *= b->step;
        }
        return;
    }
    syllable_t syl = b->notes_left == 1 ? b->last_syl : b->syl;
    float t = (float)b->pos / (float)len;
    b->phase += TWO_PI * (syl == SYL_TRILL ? b->trill_hz : 6.0f) * (float)frames / s->fs;
    float w = TWO_PI * bird_freq(b, syl, t) / s->fs;
    float c = cosf(w), sn = sinf(w);
    float t_end = (float)(b->pos + frames) / (float)len;
    t_end = t_end > 1.0f ? 1.0f : t_end;
    float env_end = note_env(t_end);
    if (syl == SYL_TRILL) {
        env_end *= 0.65f + 0.35f * sinf(b->phase);
    }
    float env = b->env_prev, step = (env_end - env) / (float)frames;
    float re = b->re, im = b->im, lp = b->lp;
    float h = b->harm;
    for (size_t k = 0; k < frames; k++) {
        float nre = re * c - im * sn;
        im = re * sn + im * c;
        re = nre;
        // im(z^2) = 2 re im: the second harmonic, for a reedier voice.
        float y = (im + h * 2.0f * re * im) * env * b->amp;
        lp += (y - lp) * b->lp_k;
        env += step;
        out[2 * k] += lp * b->gain_l;
        out[2 * k + 1] += lp * b->gain_r;
    }
    // Rotation drifts the magnitude a little each block.
    float mag = sqrtf(re * re + im * im);
    b->re = re / mag;
    b->im = im / mag;
    b->lp = lp;
    b->env_prev = env_end;
    b->pos += (uint32_t)frames;
    if (b->pos >= len) {
        b->pos = 0;
        b->env_prev = 0.0f;
        b->in_gap = true;
        if (--b->notes_left == 0) {
            b->singing = false;
            b->until_song = (uint32_t)(rnd_range(&s->rng, (float)b->song_gap_s_min, (float)b->song_gap_s_max) * s->fs);
        }
    }
}

static void render_forest(ambience_synth_t *s, float *out, size_t frames) {
    float breeze = drift_step(&s->intensity, &s->rng);
    breeze *= breeze;
    float gust[2] = {drift_step(&s->gust[0], &s->rng) * breeze, drift_step(&s->gust[1], &s->rng) * breeze};
    for (size_t k = 0; k < frames; k++) {
        if (--s->ticks.next == 0) {
            ticks_hit(&s->ticks, &s->rng, rnd_range(&s->rng, 0.005f, 0.02f), 1.0f);
            s->ticks.next = poisson_gap(s, 5.0f + FOREST_RUSTLE_HZ * breeze);
        }
        for (int ch = 0; ch < 2; ch++) {
            float w = rnd_bi(&s->rng);
            float leaves = bq_run(&s->wash_lp[ch], bq_run(&s->wash_hp[ch], pink_run(&s->pink[ch], w)));
            float low = bq_run(&s->low_lp[ch], brown_run(&s->brown[ch], w));
            float rustle = bq_run(&s->tick_f[ch], ticks_out(&s->ticks, ch, &s->rng));
            out[2 * k + ch] = leaves * 0.02f * (0.15f + gust[ch]) + low * 0.06f * gust[ch] + rustle * breeze;
        }
        ticks_advance(&s->ticks);
    }
    for (int i = 0; i < FOREST_BIRDS; i++) {
        bird_render(s, &s->birds[i], out, frames);
    }
    for (size_t k = 0; k < frames * 2; k++) {
        out[k] = soft_clip(out[k] * FOREST_GAIN);
    }
}

// -- stream --

// A brook's voice is its bubbles: air trapped as water tumbles over stones
// rings at a pitch set by the bubble's size, rising as it nears the
// surface, and they come in bursts.
#define STREAM_BUBBLE_HZ 70.0f

static void stream_reset(ambience_synth_t *s) {
    float fs = s->fs;
    bq_set_both(s->wash_hp, BQ_HIGHPASS, fs, 250.0f, 0.707f);
    bq_set_both(s->wash_lp, BQ_LOWPASS, fs, 3500.0f, 0.707f);
    bq_set_both(s->low_lp, BQ_LOWPASS, fs, 200.0f, 0.707f);
    bq_set_both(s->bus_hp, BQ_HIGHPASS, fs, 60.0f, 0.707f);
    for (int ch = 0; ch < 2; ch++) {
        drift_init(&s->gust[ch], s, 0.8f, 1.0f, 1.0f, 4.0f, 0.8f);
    }
    // The flow surging and easing over a few seconds.
    drift_init(&s->intensity, s, 0.6f, 1.3f, 0.3f, 2.5f, 0.4f);
    s->next_bubble = poisson_gap(s, STREAM_BUBBLE_HZ);
}

static void stream_bubble(ambience_synth_t *s, float flow) {
    // Bigger bubbles: lower, slower to die, louder.
    float size = rnd_uni(&s->rng);
    float freq = 2400.0f * powf(300.0f / 2400.0f, size);
    float u = rnd_uni(&s->rng);
    partial_start(s, freq, 0.004f + 0.016f * size, (0.03f + 0.09f * u * u) * (0.6f + 0.6f * size),
                  rnd_bi(&s->rng) * 0.75f, rnd_range(&s->rng, 1.15f, 1.9f));
    // Bubbles come in clusters, then a lull.
    bool burst = rnd_uni(&s->rng) < 0.35f;
    s->next_bubble = burst ? (uint32_t)(rnd_range(&s->rng, 0.004f, 0.03f) * s->fs)
                           : poisson_gap(s, STREAM_BUBBLE_HZ * flow);
}

static void render_stream(ambience_synth_t *s, float *out, size_t frames) {
    float flow = drift_step(&s->intensity, &s->rng);
    float g[2] = {drift_step(&s->gust[0], &s->rng), drift_step(&s->gust[1], &s->rng)};
    for (size_t k = 0; k < frames; k++) {
        if (--s->next_bubble == 0) {
            stream_bubble(s, flow);
        }
        for (int ch = 0; ch < 2; ch++) {
            float w = rnd_bi(&s->rng);
            float wash = bq_run(&s->wash_lp[ch], bq_run(&s->wash_hp[ch], pink_run(&s->pink[ch], w)));
            float low = bq_run(&s->low_lp[ch], brown_run(&s->brown[ch], w));
            out[2 * k + ch] = wash * 0.012f * flow * g[ch] + low * 0.08f;
        }
    }
    partials_render(s, out, frames);
    for (size_t k = 0; k < frames; k++) {
        for (int ch = 0; ch < 2; ch++) {
            out[2 * k + ch] = soft_clip(bq_run(&s->bus_hp[ch], out[2 * k + ch]) * STREAM_GAIN);
        }
    }
}

// -- fireplace --

#define FIRE_CRACKLE_HZ 5.0f
#define FIRE_POP_HZ     0.25f

static void fireplace_reset(ambience_synth_t *s) {
    float fs = s->fs;
    bq_set_both(s->low_lp, BQ_LOWPASS, fs, 350.0f, 0.707f);
    bq_set_both(s->wash_hp, BQ_HIGHPASS, fs, 3000.0f, 0.707f);
    bq_set_both(s->tick_f, BQ_HIGHPASS, fs, 900.0f, 0.707f);
    bq_set_both(s->thump_f, BQ_BANDPASS, fs, 700.0f, 1.2f);
    bq_set_both(s->bus_hp, BQ_HIGHPASS, fs, 45.0f, 0.707f);
    // The flames breathing, and flickering on top.
    drift_init(&s->intensity, s, 0.55f, 1.0f, 0.5f, 3.0f, 0.4f);
    drift_init(&s->flicker, s, 0.3f, 1.0f, 0.05f, 0.4f, 0.05f);
    ticks_init(&s->ticks, s, 0.00025f);
    ticks_init(&s->thumps, s, 0.004f);
    s->ticks.next = poisson_gap(s, FIRE_CRACKLE_HZ);
    s->thumps.next = poisson_gap(s, FIRE_POP_HZ);
}

static void fireplace_events(ambience_synth_t *s) {
    if (--s->ticks.next == 0) {
        float u = rnd_uni(&s->rng);
        ticks_hit(&s->ticks, &s->rng, 0.08f + 0.9f * u * u * u, 0.6f);
        // An ember splitting crackles in a quick run.
        s->ticks.next = rnd_uni(&s->rng) < 0.55f ? (uint32_t)(rnd_range(&s->rng, 0.003f, 0.05f) * s->fs)
                                                 : poisson_gap(s, FIRE_CRACKLE_HZ);
    }
    if (--s->thumps.next == 0) {
        ticks_hit(&s->thumps, &s->rng, rnd_range(&s->rng, 0.3f, 0.7f), 0.5f);
        s->thumps.next = poisson_gap(s, FIRE_POP_HZ);
    }
}

static void render_fireplace(ambience_synth_t *s, float *out, size_t frames) {
    float breath = drift_step(&s->intensity, &s->rng);
    float flicker = drift_step(&s->flicker, &s->rng);
    for (size_t k = 0; k < frames; k++) {
        fireplace_events(s);
        for (int ch = 0; ch < 2; ch++) {
            float w = rnd_bi(&s->rng);
            float roar = bq_run(&s->low_lp[ch], brown_run(&s->brown[ch], w)) * 0.55f * breath;
            float hiss = bq_run(&s->wash_hp[ch], pink_run(&s->pink[ch], w)) * 0.012f * flicker;
            float crackle = bq_run(&s->tick_f[ch], ticks_out(&s->ticks, ch, &s->rng));
            float pop = bq_run(&s->thump_f[ch], ticks_out(&s->thumps, ch, &s->rng));
            out[2 * k + ch] = soft_clip(bq_run(&s->bus_hp[ch], roar + hiss + crackle + pop) * FIREPLACE_GAIN);
        }
        ticks_advance(&s->ticks);
        ticks_advance(&s->thumps);
    }
}

// -- wind --

static void wind_reset(ambience_synth_t *s) {
    float fs = s->fs;
    bq_set_both(s->low_lp, BQ_LOWPASS, fs, 150.0f, 0.707f);
    bq_set_both(s->bus_hp, BQ_HIGHPASS, fs, 40.0f, 0.707f);
    // The gust shared by both sides; each side's own share of it.
    drift_init(&s->intensity, s, 0.1f, 1.0f, 2.0f, 8.0f, 1.6f);
    for (int ch = 0; ch < 2; ch++) {
        drift_init(&s->gust[ch], s, 0.7f, 1.0f, 1.0f, 4.0f, 0.8f);
    }
    // The whistle's pitch, wandering slowly.
    drift_init(&s->flicker, s, 500.0f, 1100.0f, 3.0f, 9.0f, 2.0f);
}

static void render_wind(ambience_synth_t *s, float *out, size_t frames) {
    float g = drift_step(&s->intensity, &s->rng);
    float side[2] = {drift_step(&s->gust[0], &s->rng), drift_step(&s->gust[1], &s->rng)};
    float whistle_hz = drift_step(&s->flicker, &s->rng);
    // Stronger wind sounds higher, and only a strong one whistles.
    for (int ch = 0; ch < 2; ch++) {
        float level = g * side[ch];
        bq_set(&s->wash_lp[ch], BQ_BANDPASS, s->fs, 220.0f + 700.0f * level, 0.7f);
        bq_set(&s->tick_f[ch], BQ_BANDPASS, s->fs, whistle_hz * (0.9f + 0.2f * level), 14.0f);
    }
    float whistle = g > 0.55f ? (g - 0.55f) * (g - 0.55f) * 2.5f : 0.0f;
    for (size_t k = 0; k < frames; k++) {
        for (int ch = 0; ch < 2; ch++) {
            float level = g * side[ch];
            float w = rnd_bi(&s->rng);
            float x = pink_run(&s->pink[ch], w);
            float body = bq_run(&s->wash_lp[ch], x) * 0.06f * level;
            float tone = bq_run(&s->tick_f[ch], x) * whistle;
            float low = bq_run(&s->low_lp[ch], brown_run(&s->brown[ch], w)) * 0.25f * level;
            out[2 * k + ch] = soft_clip(bq_run(&s->bus_hp[ch], body + tone + low) * WIND_GAIN);
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
    ambience_synth_reset(s, AMBIENCE_RAIN);
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
    case AMBIENCE_RAIN:
    case AMBIENCE_STORM:
        rain_reset(s, kind == AMBIENCE_STORM);
        break;
    case AMBIENCE_OCEAN:
        ocean_reset(s);
        break;
    case AMBIENCE_FOREST:
        forest_reset(s);
        break;
    case AMBIENCE_STREAM:
        stream_reset(s);
        break;
    case AMBIENCE_FIREPLACE:
        fireplace_reset(s);
        break;
    case AMBIENCE_WIND:
        wind_reset(s);
        break;
    default:
        break;
    }
}

void ambience_synth_render(ambience_synth_t *s, float *out, size_t frames) {
    while (frames > 0) {
        size_t n = frames < CTRL_FRAMES ? frames : CTRL_FRAMES;
        switch (s->kind) {
        case AMBIENCE_RAIN:
        case AMBIENCE_STORM:
            render_rain(s, out, n);
            break;
        case AMBIENCE_OCEAN:
            render_ocean(s, out, n);
            break;
        case AMBIENCE_FOREST:
            render_forest(s, out, n);
            break;
        case AMBIENCE_STREAM:
            render_stream(s, out, n);
            break;
        case AMBIENCE_FIREPLACE:
            render_fireplace(s, out, n);
            break;
        case AMBIENCE_WIND:
            render_wind(s, out, n);
            break;
        default:
            memset(out, 0, n * 2 * sizeof(float));
            break;
        }
        out += 2 * n;
        frames -= n;
    }
}
