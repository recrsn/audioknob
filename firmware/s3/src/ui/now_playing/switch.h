#pragma once

// The source picker: its own page rather than an inline toggle
// (mock-react's SourceSwitchScreen). Picking a source reports it to the
// handler from ui_set_source_handler() and opens its now-playing screen.

#include "lvgl.h"

void switch_build(void);
lv_obj_t *switch_screen(void);
void switch_open(void);
