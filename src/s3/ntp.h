#pragma once

#include <stdbool.h>

// SNTP client over this MCU's WiFi (call after wifi_start()). SNTP sets the
// system clock itself; between syncs it free-runs off the internal RTC
// (spec.md Mode C). Time is UTC -- timezone is a display concern.
void ntp_start(void);

// True once at least one sync has landed.
bool ntp_is_synced(void);
