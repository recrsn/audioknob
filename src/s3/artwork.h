#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARTWORK_SIZE 132

// Decodes a baseline JPEG into a ARTWORK_SIZE x ARTWORK_SIZE RGB565 bitmap
// (host byte order, as LVGL expects), stretched to fill. Returns false for
// anything the ROM decoder can't handle (progressive, CMYK, corrupt).
bool artwork_decode(const uint8_t *jpeg, size_t len, uint16_t out[ARTWORK_SIZE * ARTWORK_SIZE]);
