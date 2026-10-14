#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "artwork.h"

// Decodes a non-interlaced PNG straight into an ARTWORK_SIZE^2 RGB565
// bitmap, averaged down (or stretched) to fill.
bool artwork_png_decode(const uint8_t *data, size_t len, uint16_t *out);
