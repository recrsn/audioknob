#pragma once

#include <stdbool.h>

#include "protocol.h"

// The S3 end of the inter-MCU UART (spec.md §2 / lib/common/protocol.h).
// Receives the ESP32's A2DP state into shared structs and sends it
// commands. Starts a background RX task; call once from app_main.
//
// The RX task only writes into its own shared state -- it never touches
// ui.c -- so the LVGL task polls uart_link_get_latest() on its own tick.
// Neither task blocks on the other.
void uart_link_start(void);

// Copies the most recently received state into *out. Returns false (and
// leaves *out untouched) if no frame has been received yet.
bool uart_link_get_latest(wireless_state_t *out);

// Copies the most recently received MSG_TRACK_INFO frame into *out. Returns
// false (and leaves *out untouched) if no track frame has arrived yet,
// e.g. nothing has played since boot.
bool uart_link_get_latest_track(track_info_t *out);

// The ESP32's current Bluetooth cover art as an ARTWORK_SIZE^2 RGB565
// bitmap, or NULL if there is none. version changes whenever it does.
const uint16_t *uart_link_get_artwork(uint32_t *version);

// Sends a MSG_COMMAND frame to the ESP32. Fire-and-forget: there is no
// ACK; the effect shows up in the next STATE_UPDATE. Safe from any task.
void uart_link_send_command(protocol_cmd_t cmd);

// Sends the Bluetooth device name now, and again whenever the ESP32
// reappears on the link after a reset. Safe from any task.
void uart_link_set_device_name(const char *name);

// Turns Bluetooth on or off on the ESP32, now and whenever it reappears
// (it sleeps while off and asks on waking). Safe from any task.
void uart_link_set_bluetooth(bool enabled);
