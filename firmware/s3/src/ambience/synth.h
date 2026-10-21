#pragma once

#include <stddef.h>
#include <stdint.h>

// Ambient sound synthesized sample by sample from seeded noise, so it never
// repeats: there is no stored loop to hear wrap around. Plain C with no
// ESP-IDF dependency, so it can be rendered and checked off the device.

typedef enum {
    AMBIENCE_BROWN, // integrated noise: a deep, even rumble
    AMBIENCE_PINK,  // -3 dB/octave: brighter, like a steady fan
    AMBIENCE_RAIN,  // a filtered-noise wash plus random droplets
    AMBIENCE_OCEAN,  // surf: waves swelling, breaking and draining back
    AMBIENCE_FOREST, // wind in the leaves and birdsong
    AMBIENCE_COUNT,
} ambience_kind_t;

typedef struct ambience_synth ambience_synth_t;

// One synth per player; the state is a few KB, from the heap.
ambience_synth_t *ambience_synth_create(uint32_t sample_rate, uint32_t seed);

// Starts `kind` afresh (its voices and filters cleared).
void ambience_synth_reset(ambience_synth_t *s, ambience_kind_t kind);

// Writes `frames` of interleaved stereo, full scale 1.0, at about -18 dBFS
// RMS with peaks well under full scale.
void ambience_synth_render(ambience_synth_t *s, float *out, size_t frames);
