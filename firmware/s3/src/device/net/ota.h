#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Firmware updates for both chips, told apart by the image's chip id. An
// S3 image is written to the spare OTA slot, an ESP32 one to the esp32_fw
// staging partition; either is checked, then waits for the user to confirm
// on the device. Installing an S3 image makes it the boot slot and
// restarts; an ESP32 one is sent over the inter-MCU UART (esp32_fw.c). A
// new image boots on probation on either chip: on the S3, unless
// ota_confirm_running() runs, the next reset goes back to the old one.

typedef enum {
    OTA_IDLE,
    OTA_RECEIVING,
    OTA_READY,      // written and checked, waiting for ota_install()
    OTA_DECLINED,   // the user said no on the device
    OTA_INSTALLING, // ESP32 only: sending it, then waiting for it to restart
    OTA_INSTALLED,  // the ESP32 runs the new image, or the S3 kept it
    OTA_FAILED,
} ota_state_t;

typedef enum {
    OTA_TARGET_S3,
    OTA_TARGET_ESP32,
} ota_target_t;

typedef struct {
    ota_state_t state;
    ota_target_t target;
    uint8_t progress; // percent received while RECEIVING, sent while INSTALLING
    char version[32]; // of the image, once its header is in
    char error[48];   // FAILED
} ota_status_t;

// Reads up to `len` bytes of the image: > 0 bytes read, 0 at the end, < 0 on
// error.
typedef int (*ota_read_fn)(void *ctx, char *buf, size_t len);

// Receives a `size`-byte image into the spare slot, blocking until it's all
// written and checked. Audio stays paused through confirmation and install.
// On failure, false with the reason in the status; playback is allowed again.
bool ota_receive(size_t size, ota_read_fn read, void *ctx);

// Reserves the receive slot for a download, so uploads and on-device
// confirmation can't race it. False if an update is pending or busy.
bool ota_download_reserve(ota_target_t target);
// Runs a reserved HTTPS download into the same validation/confirmation path,
// on the caller's task: it writes flash, so that task's stack must be internal.
// Verifies the full download against the release asset SHA-256 before READY.
void ota_download(const char *url, size_t size, ota_target_t target, const uint8_t sha256[32]);

void ota_get_status(ota_status_t *out);

// Installs the image waiting in OTA_READY on a background task: the S3
// image by restarting into it, the ESP32 one by relaying it.
void ota_install(void);
void ota_decline(void);

// Keeps an image that's booting for the first time after an update; call
// once it has shown it can take the next one (the web server is up). The
// first time, the status reads INSTALLED with the running version.
void ota_confirm_running(void);
