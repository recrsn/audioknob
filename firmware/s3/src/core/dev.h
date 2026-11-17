#pragma once

// Unattended-testing switches. tools/dev_flash.py writes core/dev_local.h
// (gitignored) for the length of a dev build and removes it after, so a
// plain build never has them. AK_DEV_AUTO_ACCEPT: firmware updates and Talk
// calls go ahead without a press on the device.
#if __has_include("core/dev_local.h")
#include "core/dev_local.h"
#endif
