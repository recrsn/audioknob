#include "protocol.h"

#include <string.h>

// CRC-8/MAXIM (poly 0x31, init 0x00) — matches common Dallas/Maxim 1-wire CRC8,
// good enough error detection for a short on-board UART trace.
uint8_t protocol_crc8(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0x00;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

size_t protocol_encode(uint8_t type, const void *payload, uint8_t len, uint8_t *out) {
    out[0] = PROTOCOL_SYNC_BYTE;
    out[1] = type;
    out[2] = len;
    memcpy(&out[3], payload, len);
    out[3 + len] = protocol_crc8(&out[1], 2 + len);
    return 3 + len + 1;
}

enum {
    RX_WAIT_SYNC,
    RX_WAIT_TYPE,
    RX_WAIT_LEN,
    RX_WAIT_PAYLOAD,
    RX_WAIT_CRC,
};

void protocol_parser_init(protocol_parser_t *parser) {
    parser->state = RX_WAIT_SYNC;
}

bool protocol_parser_in_frame(const protocol_parser_t *parser) {
    return parser->state != RX_WAIT_SYNC;
}

protocol_feed_result_t protocol_parser_feed(protocol_parser_t *parser, uint8_t byte) {
    switch (parser->state) {
    case RX_WAIT_SYNC:
        if (byte == PROTOCOL_SYNC_BYTE) {
            parser->state = RX_WAIT_TYPE;
        }
        break;

    case RX_WAIT_TYPE:
        parser->type = byte;
        parser->state = RX_WAIT_LEN;
        break;

    case RX_WAIT_LEN:
        parser->len = byte;
        parser->idx = 0;
        parser->state = byte > 0 ? RX_WAIT_PAYLOAD : RX_WAIT_CRC;
        break;

    case RX_WAIT_PAYLOAD:
        parser->payload[parser->idx++] = byte;
        if (parser->idx == parser->len) {
            parser->state = RX_WAIT_CRC;
        }
        break;

    case RX_WAIT_CRC: {
        // CRC covers TYPE+LEN+PAYLOAD; protocol_crc8 wants them contiguous.
        uint8_t scratch[2 + UINT8_MAX];
        scratch[0] = parser->type;
        scratch[1] = parser->len;
        memcpy(&scratch[2], parser->payload, parser->len);
        parser->state = RX_WAIT_SYNC;
        return protocol_crc8(scratch, 2 + parser->len) == byte ? PROTOCOL_FEED_FRAME : PROTOCOL_FEED_BAD_CRC;
    }
    }
    return PROTOCOL_FEED_PENDING;
}
