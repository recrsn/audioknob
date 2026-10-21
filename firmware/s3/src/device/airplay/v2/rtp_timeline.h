#pragma once

/*
 * RTP timestamps are 32-bit, but a buffered stream's anchor is set once
 * (SETRATEANCHORTIME) and never refreshed, so a session can outrun 2^31
 * frames from it: 12 h 25 min at 48 kHz. Measured as an int32 difference,
 * every frame past that is timed a day early and dropped (shairport-sync
 * #2312). This extends each timestamp against the last one seen, which is
 * always far closer than 2^31 frames, into a 64-bit distance from the
 * anchor.
 */

#include <stdint.h>

typedef struct {
    uint32_t ref_rtp;     // the last timestamp extended
    int64_t ref_frames;   // its distance from the anchor
} rtp_timeline_t;

static inline void rtp_timeline_reset(rtp_timeline_t *t, uint32_t anchor_rtp) {
    t->ref_rtp = anchor_rtp;
    t->ref_frames = 0;
}

// Frames from the anchor to rtp, which may be before it.
static inline int64_t rtp_timeline_frames(rtp_timeline_t *t, uint32_t rtp) {
    int64_t frames = t->ref_frames + (int32_t)(rtp - t->ref_rtp);
    t->ref_rtp = rtp;
    t->ref_frames = frames;
    return frames;
}

// Whole seconds first: frames * 1e9 alone overflows int64 after ~53 h.
static inline int64_t rtp_frames_to_ns(int64_t frames, uint32_t rate) {
    return frames / rate * 1000000000LL + frames % rate * 1000000000LL / rate;
}
