#pragma once

/*
 * One RTSP connection from a sender, after shairport-sync's rtsp_conn_info,
 * trimmed to what an AirPlay 2 receiver needs.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pair.h"

// A ChaCha20-Poly1305 framed byte stream (pair_ap's cipher) over a socket.
typedef struct {
    struct pair_cipher_context *ctx;
    uint8_t *cipher;  // received, not yet a whole block
    size_t cipher_len, cipher_cap;
    uint8_t *plain;  // decrypted, not yet consumed
    size_t plain_len, plain_pos;
} cipher_stream_t;

typedef enum {
    CATEGORY_NONE,
    CATEGORY_PTP,             // a full audio session
    CATEGORY_REMOTE_CONTROL,  // isRemoteControlOnly
} category_t;

typedef struct event_channel event_channel_t;

typedef struct ap2_conn {
    int fd;
    int id;
    volatile bool stop;
    char client_ip[16];
    char self_ip[16];
    uint16_t client_port;
    char client_name[64];

    struct pair_setup_context *setup_ctx;
    struct pair_verify_context *verify_ctx;
    uint8_t shared_secret[64];
    size_t shared_secret_len;
    // Encryption starts after the response that completes pairing.
    struct pair_cipher_context *pending_control_cipher;
    cipher_stream_t control;
    bool encrypted;
    // Encryption is on but nothing has been read through it yet.
    bool cipher_unconfirmed;

    // AirPlay 1 (RAOP) sessions: what ANNOUNCE set up.
    bool airplay1;
    struct {
        uint8_t aeskey[16], aesiv[16];
        bool encrypted;
        char *rtpmap, *fmtp;
        char dacp_id[32], active_remote[32];
        uint32_t min_latency, max_latency;  // SDP, frames; 0 if absent
        int version;                        // User-Agent's AirPlay/<version>, 0 if none
    } ap1;

    category_t category;
    event_channel_t *events;
    uint16_t event_port;
    char group_uuid[48];
    bool group_contains_leader;
} ap2_conn_t;

// Reads up to len plaintext bytes; 0 on close, -1 on error or stop.
ssize_t cipher_stream_read(int fd, cipher_stream_t *cs, void *buf, size_t len, volatile bool *stop);
// Encrypts and writes all of buf; false on failure.
bool cipher_stream_write(int fd, cipher_stream_t *cs, const void *buf, size_t len);
void cipher_stream_free(cipher_stream_t *cs);
