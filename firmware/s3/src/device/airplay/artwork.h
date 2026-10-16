#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARTWORK_SIZE 132

// One source's current cover art: a double-buffered ARTWORK_SIZE^2 RGB565
// bitmap (host byte order, as LVGL expects), so a new decode never
// overwrites the bitmap the UI is drawing from. Thread-safe.
typedef struct artwork_slot artwork_slot_t;

artwork_slot_t *artwork_slot_create(void);

// Decodes a baseline JPEG or a non-interlaced PNG (told apart by signature),
// stretched to fill, and makes it current. Returns false (keeping the
// previous art) for anything the decoders can't handle, such as progressive
// JPEGs. len 0 clears the slot.
bool artwork_slot_set(artwork_slot_t *slot, const uint8_t *image, size_t len);

void artwork_slot_clear(artwork_slot_t *slot);

// Current bitmap, or NULL if the slot is empty. version changes whenever the
// bitmap does and is unique across all slots, so a UI switching between
// sources always sees a change.
const uint16_t *artwork_slot_get(artwork_slot_t *slot, uint32_t *version);
