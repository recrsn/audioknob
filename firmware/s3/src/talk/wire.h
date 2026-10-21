#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Opus RTP (RFC 7587): payload 96, 48 kHz timestamp clock, mono PCM16k.
#define TALK_PAYLOAD 1275
#define TALK_FRAME_SAMPLES 320
#define TALK_JITTER_FRAMES 8
#define TALK_RTP_HEADER 12
typedef struct {
    uint32_t ssrc, timestamp;
    uint16_t sequence, length;
    uint8_t payload[TALK_PAYLOAD];
} talk_packet_t;
size_t talk_rtp_encode(uint8_t *out, const talk_packet_t *packet);
bool talk_rtp_decode(talk_packet_t *packet, const uint8_t *data, size_t length);
typedef struct {
    uint16_t sequence, length;
    uint8_t payload[TALK_PAYLOAD];
    bool present;
} talk_jitter_slot_t;
typedef struct {
    uint16_t next;
    talk_jitter_slot_t slots[TALK_JITTER_FRAMES];
} talk_jitter_t;
void talk_jitter_init(talk_jitter_t *jitter, uint16_t first);
bool talk_jitter_push(talk_jitter_t *jitter, const talk_packet_t *packet);
size_t talk_jitter_pop(talk_jitter_t *jitter, uint8_t *payload);
