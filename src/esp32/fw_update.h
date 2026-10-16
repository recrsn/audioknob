#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "protocol.h"

// Firmware updates relayed by the S3 over the inter-MCU UART (MSG_FW_*,
// see fw_begin_t). The image goes into the spare OTA slot; a new image
// boots on probation and restarts (so the bootloader rolls back) unless the
// S3 answers its MSG_HELLO within a minute.

// Starts the probation timer if this boot is a new image's first.
void fw_update_init(void);

// Handles one MSG_FW_* frame on the UART RX task and fills in its ack.
// True once the new image is the boot slot: send the ack, then call
// fw_update_restart().
bool fw_update_handle(uint8_t type, const uint8_t *payload, uint8_t len, fw_ack_t *ack);

void fw_update_restart(void) __attribute__((noreturn));

// The S3 heard this MCU: keeps an image that's on probation.
void fw_update_confirm(void);

// An update is arriving or the running image is on probation; this MCU
// must stay awake meanwhile.
bool fw_update_busy(void);
