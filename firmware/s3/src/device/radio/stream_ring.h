#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

// The compressed stream between the network task (one writer) and the
// decoder (one reader), in PSRAM. Each new stream starts a new epoch, which
// tells the reader to drop its decoder state; the format travels with it.

typedef struct stream_ring stream_ring_t;

stream_ring_t *stream_ring_create(size_t size);
void stream_ring_destroy(stream_ring_t *ring);

// Empties it and starts a new epoch carrying `format`; returns the epoch.
uint32_t stream_ring_reset(stream_ring_t *ring, uint32_t format);

// Copies as much as fits, waiting up to `wait` for room; returns bytes
// written.
size_t stream_ring_write(stream_ring_t *ring, const uint8_t *data, size_t len, TickType_t wait);

// Copies up to `max` bytes out, waiting up to `wait` for any. `epoch` and
// `format` are the current epoch's, read under the same lock, so data
// never comes back labelled with a newer epoch than its own.
size_t stream_ring_read(stream_ring_t *ring, uint8_t *out, size_t max, TickType_t wait, uint32_t *epoch,
                        uint32_t *format);

size_t stream_ring_level(stream_ring_t *ring);
size_t stream_ring_size(const stream_ring_t *ring);

// Releases any writer or reader blocked on it, so they recheck their stop
// conditions.
void stream_ring_wake(stream_ring_t *ring);
