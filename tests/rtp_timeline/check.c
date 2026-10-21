#include <assert.h>
#include <stdio.h>

#include "rtp_timeline.h"

// Steps through a session of the given length one block at a time, as the
// buffered decoder does, and checks the distance never wraps.
static void run(uint32_t anchor, int64_t total, uint32_t block) {
    rtp_timeline_t t;
    rtp_timeline_reset(&t, anchor);
    for (int64_t f = 0; f <= total; f += block) {
        assert(rtp_timeline_frames(&t, anchor + (uint32_t)f) == f);
    }
}

int main(void) {
    // 30 h at 48 kHz crosses 2^31 and 2^32 frames from the anchor.
    run(0, 30LL * 3600 * 48000, 1024 * 4096);
    run(0xFFFFF000u, 30LL * 3600 * 48000, 352 * 4096);

    // Blocks before the anchor (a late one, or the gap fill) and going back.
    rtp_timeline_t t;
    rtp_timeline_reset(&t, 100);
    assert(rtp_timeline_frames(&t, 50) == -50);
    for (int64_t f = 0; f < 3000000000LL; f += 1000000000) {
        rtp_timeline_frames(&t, 100 + (uint32_t)f);
    }
    assert(rtp_timeline_frames(&t, 100 + 3000000000u) == 3000000000LL);
    assert(rtp_timeline_frames(&t, 100 + 3000000000u - 1024) == 3000000000LL - 1024);

    // Past ~53 h the naive frames * 1e9 overflows; this must not.
    int64_t frames = 100LL * 3600 * 48000;
    assert(rtp_frames_to_ns(frames, 48000) == 100LL * 3600 * 1000000000LL);
    assert(rtp_frames_to_ns(-48000 - 24000, 48000) == -1500000000LL);
    assert(rtp_frames_to_ns(1, 44100) == 22675);

    puts("rtp_timeline ok");
    return 0;
}
