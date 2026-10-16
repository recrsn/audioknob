#pragma once

#include <stdbool.h>

// Device page on the home network (http://<hostname>.local/ or the IP):
// every setting, the device name (typing on the round screen isn't
// practical), now playing with transport and volume, and firmware updates.
// A static page (web/index.html) over a small JSON API. Not up in WiFi
// setup mode, where the captive portal owns port 80.

// Starts the HTTP server; false if it couldn't. Idempotent, so it can run on
// every got-IP.
bool web_settings_start(void);
