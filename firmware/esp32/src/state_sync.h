#pragma once

#include <stdint.h>

#include "protocol.h"

// This MCU's end of the inter-MCU UART. Periodically pushes its
// wireless_state_t to the S3, per the state-sync protocol (spec.md §2):
// ~33ms on change, unconditional resync every ~1s. Also receives the S3's
// MSG_COMMAND frames and hands each to on_command, and MSG_DEVICE_NAME
// frames (NUL-terminated) to on_device_name; both run on the RX task.
// Starts both tasks and owns the UART port — call before
// state_sync_send_track().
typedef void (*state_sync_command_cb_t)(protocol_cmd_t cmd);
typedef void (*state_sync_name_cb_t)(const char *name);

void state_sync_start(state_sync_command_cb_t on_command, state_sync_name_cb_t on_device_name);

// Sends MSG_HELLO, which state_sync_start() and every wake also do.
void state_sync_send_hello(void);

// Light-sleeps until the S3 sends something (that frame is lost), then
// says hello so the S3 repeats what it needs this MCU to know.
void state_sync_light_sleep(void);

// Sends a one-off MSG_TRACK_INFO frame. Call on a genuine track change
// (AVRCP GetElementAttributes / MPRIS metadata event), not on a timer —
// unlike wireless_state_t there's no polling cadence or resync for this.
void state_sync_send_track(const track_info_t *track);

// Cover art frames (see art_chunk_t in protocol.h). len <= ART_CHUNK_MAX_DATA.
// total_len 0 tells the S3 to clear its art.
void state_sync_send_art_chunk(uint8_t id, uint32_t offset, const uint8_t *data, uint8_t len);
void state_sync_send_art_end(uint8_t id, uint32_t total_len);
