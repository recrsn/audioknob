#pragma once

/*
 * Internals shared across the AirPlay 2 receiver (after
 * shairport-sync). receiver.c implements raop.h on top of these.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "raop.h"

#define AP2_RTSP_PORT 7000
#define AP2_RATE 44100
#define AP2_MODEL "AudioKnob"
#define AP2_SRCVERS "366.0"
#define AP2_FIRMWARE "1.0"
// Audio cable attached.
#define AP2_STATUS_FLAGS 0x4
// AirPlay's volume slider spans -30..0 dB; -144 is mute.
#define AP2_VOLUME_MIN_DB (-30.0f)
#define AP2_VOLUME_MUTE_DB (-144.0f)

// Identity, fixed for the life of the firmware (receiver.c).
const char *ap2_device_id(void);   // "AA:BB:CC:DD:EE:FF"
const uint8_t *ap2_public_key(void);  // Ed25519, 32 bytes
const char *ap2_public_key_hex(void);
const char *ap2_pi(void);
const char *ap2_psi(void);
const char *ap2_fex(void);  // features, base64, no padding
uint64_t ap2_features(void);
void ap2_name(char *out, size_t len);

// Whether to offer 24-bit/48 kHz ALAC buffered streams.
bool ap2_lossless(void);
// What the running stream carries, for display: a string literal, or NULL.
void ap2_set_format(const char *format);

// Last volume the sender set, in dB.
float ap2_volume_db(void);
void ap2_set_volume_db(float db);

const uint8_t *ap2_mac(void);

bool ap2_emit(const raop_event_t *event);
void ap2_emit_type(raop_event_type_t type);
void ap2_push_pcm(const int32_t *pcm, size_t frames, int64_t playtime_us, uint32_t rate);

// A bounded-wait recv() on a socket with SO_RCVTIMEO set that retries
// timeouts until stop is raised. Returns what recv() did otherwise.
int ap2_recv(int fd, void *buf, size_t len, volatile bool *stop);

// Binds a socket of this type to an ephemeral port on all interfaces.
int ap2_listen_any(int type, uint16_t *port);

// Byte buffers off the hot path go to PSRAM; internal RAM is kept for
// stacks and DMA.
void *ap2_malloc(size_t size);
void ap2_free(void *p);

// Logs how close the calling task came to overflowing its stack.
void ap2_log_stack(void);
void ap2_log_heap(void);

// Every task started here must end with ap2_task_exit().
//
// psram_stack is only for tasks that sit idle on control traffic (RTSP,
// listen, event channel): audio-path stacks in PSRAM brought back late
// packets and timing round-trips on 2026-10-04. A PSRAM-stack task must
// never reach a flash write (NVS, OTA) itself, since the cache, and with it
// PSRAM, is off for the duration; that includes the raop event callbacks
// it runs.
bool ap2_task_start(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                      BaseType_t core, bool psram_stack);
void ap2_task_exit(void) __attribute__((noreturn));
