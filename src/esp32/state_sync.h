#pragma once

#include <stdint.h>

#include "protocol.h"

// This MCU's end of the inter-MCU UART. Periodically pushes its
// wireless_state_t to the S3, per the state-sync protocol (spec.md §2):
// ~33ms on change, unconditional resync every ~1s. Also receives the S3's
// MSG_COMMAND frames and hands each to on_command, which runs on the RX
// task. Starts both tasks and owns the UART port — call before
// state_sync_send_track().
typedef void (*state_sync_command_cb_t)(protocol_cmd_t cmd);

void state_sync_start(state_sync_command_cb_t on_command);

// Sends a one-off MSG_TRACK_INFO frame. Call on a genuine track change
// (AVRCP GetElementAttributes / MPRIS metadata event), not on a timer —
// unlike wireless_state_t there's no polling cadence or resync for this.
// Not yet called anywhere: no metadata source is wired up on the BT side
// yet (see avrcp_controller.c's "no metadata/registration" note).
void state_sync_send_track(const track_info_t *track);
