/*
 * RAOP RTP receiver: audio/control/timing sockets, AES decrypt, ALAC
 * decode, reorder + resend, and sender clock tracking.
 *
 * Derived from HairTunes (c) James Laird 2011, modularised by
 * philippe_44@outlook.com 2019 -- MIT License (see raop_rtp.c).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lwip/inet.h"
#include "raop.h"

typedef struct raop_rtp_s raop_rtp_t;

typedef struct {
    uint16_t cport, tport, aport;
    raop_rtp_t *ctx;
} raop_rtp_resp_t;

// aeskey/aesiv may both be NULL for an unencrypted stream. rtpmap and fmtp
// are the SDP "a=rtpmap:" and "a=fmtp:" values: ALAC ("96 AppleLossless")
// or 16-bit PCM ("96 L16/44100/2"). ctx in the result is NULL on failure.
raop_rtp_resp_t raop_rtp_init(struct in_addr host, const uint8_t *aeskey, const uint8_t *aesiv, const char *rtpmap,
                              const char *fmtp,
                    uint16_t ctrl_port, uint16_t timing_port, raop_event_cb_t event_cb, raop_pcm_cb_t pcm_cb);
void raop_rtp_end(raop_rtp_t *ctx);

void raop_rtp_record(raop_rtp_t *ctx, uint16_t seqno, uint32_t rtptime);
// Emits RAOP_EVENT_FLUSH (under the buffer lock, so no stale frame can
// slip out after it) if playback was actually running.
void raop_rtp_flush(raop_rtp_t *ctx, uint16_t seqno, uint32_t rtptime);
