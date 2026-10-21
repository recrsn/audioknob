#pragma once

// The clock's top band: one thing asking for attention (Talk, then unread
// messages), a tap away from its page. The now-playing pill gives way to the
// same item.

#include "ui/internal.h"

typedef struct {
    int app;
    const char *icon;
    lv_color_t tint;
    char text[24];
} band_item_t;

// `app` with something in progress, as a band item.
bool band_app_item(ui_app_t app, band_item_t *out);
// What the band shows, by priority; false for nothing.
bool band_item(band_item_t *out);
// Apps that show in the band rather than as a chip of their own.
bool band_owns(ui_app_t app);

void band_build(lv_obj_t *screen);
void band_refresh(void);
