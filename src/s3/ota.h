#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// S3 firmware updates. An image is written to the spare OTA slot and
// checked, then waits for the user to confirm on the device before it's
// made the boot slot. A new image boots on probation: unless
// ota_confirm_running() runs, the next reset goes back to the old one.

typedef enum {
    OTA_IDLE,
    OTA_RECEIVING,
    OTA_READY,    // written and checked, waiting for ota_install()
    OTA_DECLINED, // the user said no on the device
    OTA_FAILED,
} ota_state_t;

typedef struct {
    ota_state_t state;
    char version[32]; // of the image received (READY, DECLINED)
    char error[48];   // FAILED
} ota_status_t;

// Reads up to `len` bytes of the image: > 0 bytes read, 0 at the end, < 0 on
// error.
typedef int (*ota_read_fn)(void *ctx, char *buf, size_t len);

// Receives a `size`-byte image into the spare slot, blocking until it's all
// written and checked. On failure, false with the reason in the status.
bool ota_receive(size_t size, ota_read_fn read, void *ctx);

void ota_get_status(ota_status_t *out);

// Boots the image waiting in OTA_READY: restarts, so it doesn't return then.
void ota_install(void);
void ota_decline(void);

// Keeps an image that's booting for the first time after an update; call
// once it has shown it can take the next one (the web server is up).
void ota_confirm_running(void);
