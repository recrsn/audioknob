#pragma once

/*
 * Follows the AirPlay 2 sender's PTP (IEEE 1588 / 802.1AS) clock the way
 * nqptp does for shairport-sync: Announce and Follow_Up on UDP 319/320,
 * with an Announce of our own now and then to wake a silent clock.
 *
 * Local time is esp_timer_get_time() in nanoseconds;
 * master time = local time + offset.
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint64_t clock_id;            // the grandmaster
    int64_t local_ns;             // when offset_ns was worked out
    int64_t offset_ns;            // master = local + offset
    int64_t clock_change_ns;      // old offset - new, at the last grandmaster change
    int64_t mastership_start_ns;  // local time this clock's timing sequence began
} ptp_clock_t;

esp_err_t ptp_init(void);

// The control messages shairport-sync sends nqptp.
// "T <ip>": follow the clock at this IPv4 address, from scratch; NULL ("T") follows none.
void ptp_set_timing_peer(const char *ip);
// "B": playing, so the clock won't sleep.
void ptp_play_begin(void);
// "E": stopped; the clock may sleep soon.
void ptp_play_end(void);

// False while there is no master clock to time against.
bool ptp_clock(ptp_clock_t *out);

int64_t ptp_local_ns(void);
