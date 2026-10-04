#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Captive portal for first-time WiFi setup: a DNS server that resolves every
// name to the setup AP, and an HTTP server whose setup page lists nearby
// networks and takes a password. Phones probing for captive portals get
// redirected to that page, so it opens by itself after joining the AP.
// Knows nothing about WiFi state -- everything goes through ops.

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_network_t;

typedef struct {
    // Fills out with up to max networks, strongest first. refresh asks for a
    // new scan rather than the cached one. Returns the count.
    size_t (*scan)(wifi_network_t *out, size_t max, bool refresh);
    // Starts trying the given credentials; the outcome shows up in status().
    void (*submit)(const char *ssid, const char *password);
    // Human-readable progress, as also shown on the device screen.
    void (*status)(char *out, size_t len);
} wifi_portal_ops_t;

// ap_ip: the setup AP's address (network byte order), which every DNS
// answer and redirect points at.
void wifi_portal_start(uint32_t ap_ip, const wifi_portal_ops_t *ops);
