#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Inter-MCU UART framing: [SYNC 0x7E][TYPE][LEN][PAYLOAD...][CRC8]
// CRC8 covers TYPE, LEN, and PAYLOAD (not SYNC — it's a fixed framing byte).
#define PROTOCOL_SYNC_BYTE 0x7E
// Same rate as Waveshare's stock firmware on this link; headroom for
// bulk payloads like cover art.
#define PROTOCOL_UART_BAUD 921600

typedef enum {
    MSG_STATE_UPDATE = 0x01,
    // 0x02 was MSG_NTP_SYNC, retired when NTP moved to the S3's own WiFi.
    MSG_TRACK_INFO = 0x03,
    MSG_COMMAND = 0x04,  // S3 -> ESP32; every other type flows ESP32 -> S3
    MSG_ART_CHUNK = 0x05,
    MSG_ART_END = 0x06,
} protocol_msg_type_t;

typedef enum {
    SOURCE_AIRPLAY = 0,
    SOURCE_A2DP = 1,
} wireless_source_t;

typedef struct __attribute__((packed)) {
    uint8_t volume;    // 0-100
    uint8_t muted;
    uint8_t connected;
    uint8_t playing;
    uint8_t source;    // wireless_source_t
} wireless_state_t;

// Sent once per track change (AVRCP/MPRIS metadata event), not on the
// wireless_state_t poll tick -- unlike volume/playback state, title/artist
// don't need a 33ms cadence or unconditional resync, just send-on-change.
// Fields are NUL-terminated and may be empty (e.g. no album tag); 192 bytes
// payload comfortably fits the protocol's uint8_t LEN.
typedef struct __attribute__((packed)) {
    char title[64];
    char artist[64];
    char album[64];
} track_info_t;

typedef enum {
    // Stop A2DP playback: AirPlay on the S3 has taken the DAC.
    CMD_PAUSE = 0x01,
    // Touch transport buttons, forwarded over AVRCP.
    CMD_PLAY_PAUSE = 0x02,
    CMD_NEXT = 0x03,
    CMD_PREV = 0x04,
} protocol_cmd_t;

typedef struct __attribute__((packed)) {
    uint8_t cmd;  // protocol_cmd_t
} command_t;

// Cover art (an AVRCP linked thumbnail, a baseline JPEG) streams as
// MSG_ART_CHUNKs in order, then one MSG_ART_END. A chunk at offset 0 starts
// a new image; the receiver drops the image on any gap or id mismatch
// rather than asking for a resend. MSG_ART_END with total_len 0 means the
// track has no artwork (or the transfer failed): clear it.
#define ART_CHUNK_HEADER 5
#define ART_CHUNK_MAX_DATA (UINT8_MAX - ART_CHUNK_HEADER)

typedef struct __attribute__((packed)) {
    uint8_t id;       // changes per image, so late chunks of an old one are ignored
    uint32_t offset;
    uint8_t data[ART_CHUNK_MAX_DATA];  // only LEN - ART_CHUNK_HEADER bytes are sent
} art_chunk_t;

typedef struct __attribute__((packed)) {
    uint8_t id;
    uint32_t total_len;
} art_end_t;

uint8_t protocol_crc8(const uint8_t *data, uint8_t len);

// SYNC + TYPE + LEN + max payload + CRC.
#define PROTOCOL_MAX_FRAME (3 + UINT8_MAX + 1)

// Writes a complete frame into out (>= PROTOCOL_MAX_FRAME bytes) and
// returns its length.
size_t protocol_encode(uint8_t type, const void *payload, uint8_t len, uint8_t *out);

// Byte-at-a-time frame parser, shared by both ends of the link.
typedef enum {
    PROTOCOL_FEED_PENDING,  // mid-frame or hunting for SYNC
    PROTOCOL_FEED_FRAME,    // type/len/payload hold a CRC-valid frame
    PROTOCOL_FEED_BAD_CRC,  // frame dropped
} protocol_feed_result_t;

typedef struct {
    uint8_t state;
    uint8_t type;
    uint8_t len;
    uint8_t idx;
    uint8_t payload[UINT8_MAX];
} protocol_parser_t;

void protocol_parser_init(protocol_parser_t *parser);
protocol_feed_result_t protocol_parser_feed(protocol_parser_t *parser, uint8_t byte);
