#pragma once

#include <stdbool.h>
#include <stddef.h>

// The ESP32's firmware, staged in the S3's esp32_fw partition and relayed
// over the inter-MCU UART (MSG_FW_*, see fw_begin_t in protocol.h). ota.c
// drives it; the functions return NULL or why they failed.

const char *esp32_fw_stage_begin(size_t size);
bool esp32_fw_stage_write(const void *buf, size_t len);
// Reads the staged image back and checks it against the digest esptool
// appends, so what gets sent is what was built.
const char *esp32_fw_stage_end(void);
void esp32_fw_stage_abort(void);

// Sends the staged image, then waits for the ESP32 to restart and report
// that it runs it (not the old one, after a rollback). Blocks for the
// whole update; progress gets the percentage sent.
const char *esp32_fw_install(void (*progress)(unsigned percent));
