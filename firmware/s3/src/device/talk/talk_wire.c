#include "talk_wire.h"
#include <string.h>
static uint16_t get16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static void put16(uint8_t *p, uint16_t n) {
    p[0] = n >> 8;
    p[1] = n;
}
static uint32_t get32(const uint8_t *p) {
    return ((uint32_t)get16(p) << 16) | get16(p + 2);
}
static void put32(uint8_t *p, uint32_t n) {
    put16(p, n >> 16);
    put16(p + 2, n);
}
size_t talk_rtp_encode(uint8_t *out, const talk_packet_t *p) {
    if (!p->length || p->length > TALK_PAYLOAD)
        return 0;
    out[0] = 0x80;
    out[1] = 96;
    put16(out + 2, p->sequence);
    put32(out + 4, p->timestamp);
    put32(out + 8, p->ssrc);
    memcpy(out + 12, p->payload, p->length);
    return 12 + p->length;
}
bool talk_rtp_decode(talk_packet_t *p, const uint8_t *data, size_t len) {
    if (len < 12 || data[0] >> 6 != 2 || (data[1] & 0x7f) != 96)
        return false;
    size_t offset = 12 + 4 * (data[0] & 15);
    if (offset > len)
        return false;
    if (data[0] & 0x10) {
        if (offset + 4 > len)
            return false;
        size_t extension = 4 + 4 * get16(data + offset + 2);
        if (extension > len - offset)
            return false;
        offset += extension;
    }
    size_t end = len;
    if (data[0] & 0x20) {
        uint8_t padding = data[len - 1];
        if (!padding || padding > len - offset)
            return false;
        end -= padding;
    }
    if (end <= offset || end - offset > TALK_PAYLOAD)
        return false;
    p->sequence = get16(data + 2);
    p->timestamp = get32(data + 4);
    p->ssrc = get32(data + 8);
    p->length = end - offset;
    memcpy(p->payload, data + offset, p->length);
    return true;
}
void talk_jitter_init(talk_jitter_t *j, uint16_t first) {
    memset(j, 0, sizeof(*j));
    j->next = first;
}
bool talk_jitter_push(talk_jitter_t *j, const talk_packet_t *p) {
    int16_t ahead = (int16_t)(p->sequence - j->next);
    if (ahead < 0 || ahead >= TALK_JITTER_FRAMES || p->length > TALK_PAYLOAD || !p->length)
        return false;
    talk_jitter_slot_t *slot = &j->slots[p->sequence % TALK_JITTER_FRAMES];
    if (slot->present && slot->sequence == p->sequence)
        return false;
    slot->sequence = p->sequence;
    slot->length = p->length;
    slot->present = true;
    memcpy(slot->payload, p->payload, p->length);
    return true;
}
size_t talk_jitter_pop(talk_jitter_t *j, uint8_t *payload) {
    talk_jitter_slot_t *slot = &j->slots[j->next % TALK_JITTER_FRAMES];
    size_t n = slot->present && slot->sequence == j->next ? slot->length : 0;
    if (n)
        memcpy(payload, slot->payload, n);
    slot->present = false;
    j->next++;
    return n;
}
