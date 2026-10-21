#pragma once

// Display sleep: with no input for the user's timeout, an opaque catcher on
// the system layer blacks out everything (which is all the emulator shows)
// and the display handler turns the backlight off. The input that wakes it
// is swallowed, so a tap or knob turn only lights the screen. ui_wake() is
// in ui/internal.h.

void ui_sleep_build(void);
// Sleeps once idle long enough, unless something must be seen.
void ui_sleep_tick(void);
