#pragma once

#include <stdbool.h>

#include "protocol.h"

// Single source of truth for this MCU's wireless_state_t, per the state-sync
// protocol (lib/common/protocol.h). a2dp_sink/avrcp_target/encoder write into
// it; state_sync reads it out to the S3 over UART. Thread-safe: called from
// the Bluedroid callback task, the encoder task, and the state-sync task.

void app_state_init(void);

void app_state_set_connected(bool connected);
void app_state_set_playing(bool playing);
void app_state_set_volume(uint8_t volume);
void app_state_set_muted(bool muted);

// Snapshot for state_sync to send. Cheap: a lock + memcpy.
wireless_state_t app_state_get(void);

// The connected phone's name (NUL-terminated UTF-8; cut at a character
// boundary past PEER_NAME_MAX bytes). "" clears it.
void app_state_set_peer_name(const char *name);
// Copies it into out, PEER_NAME_MAX + 1 bytes; "" when there's none.
void app_state_get_peer_name(char out[PEER_NAME_MAX + 1]);
