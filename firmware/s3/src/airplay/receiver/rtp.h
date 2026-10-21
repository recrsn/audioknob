#pragma once

/*
 * The UDP side of a stream: the control port every stream gets, and for
 * realtime streams the audio port, with packets the sender paces in real
 * time ~2 s ahead. AirPlay 2 realtime streams (type 96) are anchored
 * through the control port; AirPlay 1 streams add a timing port for the
 * sender's NTP clock. One task select()s over a realtime stream's ports; a
 * buffered stream's control port carries nothing it needs and isn't read.
 */

#include <stdbool.h>
#include <stdint.h>

#include "lwip/inet.h"

typedef struct rtp rtp_t;

// *data_port is only set (and audio only received) when realtime.
rtp_t *rtp_start(const uint8_t key[32], bool realtime, uint16_t *data_port, uint16_t *control_port);

// An AirPlay 1 stream, as ANNOUNCE and SETUP described it.
typedef struct {
    struct in_addr sender;
    uint16_t control_port, timing_port;  // the sender's
    const uint8_t *aeskey, *aesiv;       // both NULL when unencrypted
    const char *rtpmap, *fmtp;           // SDP: ALAC or "L16/44100/2"
    uint32_t min_latency, max_latency;   // SDP, frames; 0 if absent
    bool latency_offset;                 // shairport-sync's 11025 frames
} rtp_ap1_t;

// ports gets our control, timing and data ports.
rtp_t *rtp_start_ap1(const rtp_ap1_t *params, uint16_t ports[3]);
// RECORD or FLUSH: packets before seq don't play. Call before the session's
// flush, so nothing from before it can start the new epoch.
void rtp_ap1_flush(rtp_t *r, uint16_t seq);

void rtp_stop(rtp_t *r);
