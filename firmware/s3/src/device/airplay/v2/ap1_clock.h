#pragma once

/*
 * An AirPlay 1 stream's timing: the sender's NTP clock, measured over the
 * timing port, and the sync packets on the control port that tie RTP time
 * to it. After shairport-sync's rtp.c (rtp_timing_receiver,
 * rtp_control_receiver; Copyright (c) James Laird 2013, Mike Brady
 * 2014--2026, MIT License), keeping the lowest-delay exchange of the last
 * few rather than fitting a drift line: the player absorbs drift.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#define AP1_CLOCK_HISTORY 8
#define AP1_TIMING_REQUEST_LEN 32

typedef struct {
    portMUX_TYPE lock;
    // From ANNOUNCE: the SDP's latency bounds (0 if absent), and whether
    // the sender is one that wants shairport-sync's 11025-frame offset.
    uint32_t min_latency, max_latency;
    bool latency_offset;

    int64_t offsets[AP1_CLOCK_HISTORY], delays[AP1_CLOCK_HISTORY];
    int count, next;
    int64_t offset_us;  // sender NTP time minus esp_timer time
    bool timed;

    uint32_t latency;  // frames
    uint32_t anchor_rtp;
    int64_t anchor_remote_us;  // sender time at which anchor_rtp plays
    bool anchored;
} ap1_clock_t;

void ap1_clock_init(ap1_clock_t *c, uint32_t min_latency, uint32_t max_latency, bool latency_offset);

// A timing request stamped now; the sender echoes the stamp back.
void ap1_clock_request(uint8_t out[AP1_TIMING_REQUEST_LEN]);
// A timing reply (type 0x53) that arrived at arrival_us.
void ap1_clock_reply(ap1_clock_t *c, const uint8_t *pkt, size_t len, int64_t arrival_us);
// A sync packet (type 0x54).
void ap1_clock_sync(ap1_clock_t *c, const uint8_t *pkt, size_t len);

bool ap1_clock_timed(ap1_clock_t *c);
// The esp_timer time (us) at which this RTP frame should reach the DAC.
bool ap1_clock_playtime_us(ap1_clock_t *c, uint32_t rtp, int64_t *playtime_us);
