#include "airplay/receiver/ap1/clock.h"

#include <string.h>

#include "airplay/receiver/ap2.h"
#include "esp_log.h"
#include "esp_timer.h"

// shairport-sync's limit on a usable timing exchange.
#define MAX_ROUNDTRIP_US 200000
#define GOOD_ROUNDTRIP_US 20000
#define MIN_EXCHANGES 3
// Senders on these AirPlay versions, or sync packets flagged 7, play this
// much later than their stated latency (shairport-sync's
// fixedLatencyOffset).
#define LATENCY_OFFSET_FRAMES 11025
#define SYNC_FLAGS_OFFSET 7
// What the player's ring can hold ahead; a longer latency is ignored.
#define MAX_LATENCY_FRAMES ((120 * AP2_RATE * 2) / 100)

static const char *TAG = "ap1_clock";

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64(const uint8_t *p) {
    return (uint64_t)be32(p) << 32 | be32(p + 4);
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static int64_t ntp_to_us(uint64_t ntp) {
    return (int64_t)(ntp >> 32) * 1000000 + (int64_t)(((ntp & 0xffffffff) * 1000000) >> 32);
}

static uint64_t us_to_ntp(int64_t us) {
    return ((uint64_t)(us / 1000000) << 32) | (((uint64_t)(us % 1000000) << 32) / 1000000);
}

void ap1_clock_init(ap1_clock_t *c, uint32_t min_latency, uint32_t max_latency, bool latency_offset) {
    *c = (ap1_clock_t){
        .lock = portMUX_INITIALIZER_UNLOCKED,
        .min_latency = min_latency,
        .max_latency = max_latency,
        .latency_offset = latency_offset,
    };
}

void ap1_clock_request(uint8_t out[AP1_TIMING_REQUEST_LEN]) {
    memset(out, 0, AP1_TIMING_REQUEST_LEN);
    out[0] = 0x80;
    out[1] = 0xD2;
    out[3] = 7;
    // Our clock passed off as NTP: only differences against it matter.
    uint64_t now = us_to_ntp(esp_timer_get_time());
    put_be32(out + 24, now >> 32);
    put_be32(out + 28, (uint32_t)now);
}

void ap1_clock_reply(ap1_clock_t *c, const uint8_t *pkt, size_t len, int64_t arrival_us) {
    if (len < 32) {
        return;
    }
    int64_t origin = ntp_to_us(be64(pkt + 8));  // our request's stamp, echoed
    int64_t receive = ntp_to_us(be64(pkt + 16));
    int64_t transmit = ntp_to_us(be64(pkt + 24));
    int64_t delay = (arrival_us - origin) - (transmit - receive);
    if (delay < 0 || delay > MAX_ROUNDTRIP_US) {
        ESP_LOGW(TAG, "discarding a %lld us timing round trip", (long long)delay);
        return;
    }

    portENTER_CRITICAL(&c->lock);
    int i = c->next;
    c->offsets[i] = ((receive - origin) + (transmit - arrival_us)) / 2;
    c->delays[i] = delay;
    c->next = (i + 1) % AP1_CLOCK_HISTORY;
    if (c->count < AP1_CLOCK_HISTORY) {
        c->count++;
    }
    // The shortest round trip bounds the offset most tightly.
    int best = i;
    for (int j = 0; j < c->count; j++) {
        if (c->delays[j] < c->delays[best]) {
            best = j;
        }
    }
    c->offset_us = c->offsets[best];
    // A slow exchange can be off by half its round trip: wait for a quick
    // one, or a few to choose from, before anything plays against it.
    c->timed = c->delays[best] <= GOOD_ROUNDTRIP_US || c->count >= MIN_EXCHANGES;
    portEXIT_CRITICAL(&c->lock);
}

void ap1_clock_sync(ap1_clock_t *c, const uint8_t *pkt, size_t len) {
    if (len < 20) {
        return;
    }
    uint16_t flags = (uint16_t)(pkt[2] << 8 | pkt[3]);
    uint32_t rtp_less_latency = be32(pkt + 4);
    int64_t remote_us = ntp_to_us(be64(pkt + 8));
    uint32_t rtp = be32(pkt + 16);

    uint32_t latency = rtp - rtp_less_latency;
    if (c->latency_offset || flags == SYNC_FLAGS_OFFSET) {
        latency += LATENCY_OFFSET_FRAMES;
    }
    if (c->max_latency && latency > c->max_latency) {
        latency = c->max_latency;
    }
    if (c->min_latency && latency < c->min_latency) {
        latency = c->min_latency;
    }

    portENTER_CRITICAL(&c->lock);
    // Out of range keeps the latency already in use, as shairport-sync does.
    bool out_of_range = latency > MAX_LATENCY_FRAMES;
    bool changed = !out_of_range && (!c->anchored || c->latency != latency);
    if (!out_of_range) {
        c->latency = latency;
    }
    if (c->anchored || !out_of_range) {
        c->anchor_rtp = rtp - c->latency;
        c->anchor_remote_us = remote_us;
        c->anchored = true;
    }
    portEXIT_CRITICAL(&c->lock);

    if (out_of_range) {
        ESP_LOGW(TAG, "ignoring a %lu-frame latency", (unsigned long)latency);
    } else if (changed) {
        ESP_LOGI(TAG, "latency %lu frames", (unsigned long)latency);
    }
}

bool ap1_clock_timed(ap1_clock_t *c) {
    portENTER_CRITICAL(&c->lock);
    bool timed = c->timed;
    portEXIT_CRITICAL(&c->lock);
    return timed;
}

bool ap1_clock_playtime_us(ap1_clock_t *c, uint32_t rtp, int64_t *playtime_us) {
    portENTER_CRITICAL(&c->lock);
    bool ok = c->timed && c->anchored;
    int64_t remote = c->anchor_remote_us + (int64_t)(int32_t)(rtp - c->anchor_rtp) * 1000000 / AP2_RATE;
    int64_t offset = c->offset_us;
    portEXIT_CRITICAL(&c->lock);
    if (ok) {
        *playtime_us = remote - offset;
    }
    return ok;
}
