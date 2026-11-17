#pragma once

#include <stddef.h>
#include <stdint.h>

// Ambient sound synthesized sample by sample from seeded noise, so it never
// repeats: there is no stored loop to hear wrap around. Plain C with no
// ESP-IDF dependency, so it can be rendered and checked off the device.

// Saved by index (settings `amb_kind`/`focus_kind`, the web API): add new
// kinds at the end.
typedef enum {
    AMBIENCE_RAIN,      // a wash, a patter of impacts, bubbles and drips
    AMBIENCE_STORM,     // heavier rain and rolling thunder
    AMBIENCE_OCEAN,     // surf: waves swelling, breaking and fizzing back
    AMBIENCE_STREAM,    // a brook: water babbling over stones
    AMBIENCE_FOREST,    // a quiet breeze under resident birds' songs
    AMBIENCE_WIND,      // gusts, with a whistle when they're strong
    AMBIENCE_FIREPLACE, // a low roar, crackles and the odd pop
    AMBIENCE_COUNT,
} ambience_kind_t;

typedef struct ambience_synth ambience_synth_t;

// One synth per player; the state is a few KB, from the heap.
ambience_synth_t *ambience_synth_create(uint32_t sample_rate, uint32_t seed);

// Starts `kind` afresh (its voices and filters cleared).
void ambience_synth_reset(ambience_synth_t *s, ambience_kind_t kind);

// Writes `frames` of interleaved stereo, full scale 1.0, at about -18 dBFS
// RMS (forest a little under, being mostly quiet between songs) with peaks
// well under full scale.
void ambience_synth_render(ambience_synth_t *s, float *out, size_t frames);
