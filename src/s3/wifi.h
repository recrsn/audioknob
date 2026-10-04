#pragma once

#include <stdbool.h>
#include <stdint.h>

// WiFi for the S3 -- per spec.md's Shared Resource Arbitration the only
// radio in the system that joins a network; the ESP32 stays Bluetooth-only.
//
// With saved credentials (NVS, else CONFIG_AK_WIFI_SSID/PASSWORD) it joins
// as a station and reconnects on drop. With none -- or if they haven't
// connected within a minute of boot -- it enters setup mode: a WPA2 setup
// AP plus captive portal (wifi_portal.c) where a phone picks the home
// network. Working credentials are saved and the device restarts.

typedef void (*wifi_got_ip_cb_t)(const uint8_t mac[6]);

// on_got_ip runs on the default event loop task on every (re)acquired
// station address, so it must not block. Never called in setup mode.
void wifi_start(wifi_got_ip_cb_t on_got_ip);

typedef struct {
    char ap_ssid[33];
    char ap_password[17];
    char qr_payload[128];  // standard "WIFI:T:WPA;S:...;P:...;;" join code
    char status[96];
} wifi_setup_info_t;

// Copies the setup-mode details for display. False when not in setup mode.
bool wifi_get_setup_info(wifi_setup_info_t *out);
