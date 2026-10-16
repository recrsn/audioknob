#pragma once

#include <stdbool.h>

// SNTP client over this MCU's WiFi (call after wifi_start()). SNTP sets the
// system clock itself; between syncs it free-runs off the internal RTC
// (spec.md Mode C). Time is UTC -- timezone is a display concern.

// Marks the clock set if the RTC carried a synced time over a software
// reset. Call before the UI starts so the first frame shows it.
void ntp_restore_rtc(void);

// Starts SNTP in the background; it corrects the clock whenever a sync lands.
void ntp_start(void);

// True once the clock is set: by a sync this boot, or by one before a
// software reset that the RTC carried over.
bool ntp_is_synced(void);
