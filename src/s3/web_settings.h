#pragma once

// Device page on the home network (http://<hostname>.local/ or the IP),
// where the device name is changed after setup -- typing on the round
// screen isn't practical. Not up in WiFi setup mode, where the captive
// portal owns port 80.

// Starts the HTTP server. Idempotent, so it can run on every got-IP.
void web_settings_start(void);
