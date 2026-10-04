/*
 * RAOP RTP receiver: audio/control/timing sockets, AES decrypt, ALAC
 * decode, reorder + resend, and sender clock tracking.
 *
 * Derived from HairTunes (c) James Laird 2011, modularised by
 * philippe_44@outlook.com 2019 -- MIT License (see rtp.c).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lwip/inet.h"
#include "raop.h"

typedef struct rtp_s rtp_t;

typedef struct {
    uint16_t cport, tport, aport;
    rtp_t *ctx;
} rtp_resp_t;

// aeskey/aesiv may both be NULL for an unencrypted stream. fmtp is the SDP
// "a=fmtp:" value. ctx in the result is NULL on failure.
rtp_resp_t rtp_init(struct in_addr host, const uint8_t *aeskey, const uint8_t *aesiv, const char *fmtp,
                    uint16_t ctrl_port, uint16_t timing_port, raop_event_cb_t event_cb, raop_pcm_cb_t pcm_cb);
void rtp_end(rtp_t *ctx);

void rtp_record(rtp_t *ctx, uint16_t seqno, uint32_t rtptime);
// Emits RAOP_EVENT_FLUSH (under the buffer lock, so no stale frame can
// slip out after it) if playback was actually running.
void rtp_flush(rtp_t *ctx, uint16_t seqno, uint32_t rtptime);
