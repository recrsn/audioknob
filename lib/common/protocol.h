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
    MSG_COMMAND = 0x04,  // S3 -> ESP32
    MSG_ART_CHUNK = 0x05,
    MSG_ART_END = 0x06,
    MSG_DEVICE_NAME = 0x07,  // S3 -> ESP32; every other type flows ESP32 -> S3
    // No payload. The ESP32 just booted or woke; the S3 answers with the
    // device name and CMD_BT_ENABLE/CMD_BT_DISABLE.
    MSG_HELLO = 0x08,
    // pairing_status_t, sent like MSG_STATE_UPDATE: on change and resent
    // every ~1 s.
    MSG_PAIRING = 0x09,
} protocol_msg_type_t;

// MSG_DEVICE_NAME's payload is the name's UTF-8 bytes, no NUL, 1 to
// DEVICE_NAME_MAX of them. The S3 owns the name (it's also the AirPlay name
// and setup hotspot SSID); the ESP32 advertises it over Bluetooth. Sent on
// every change and whenever the ESP32 reappears on the link.
#define DEVICE_NAME_MAX 32

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
    // One S3 knob detent of Bluetooth volume, while A2DP is in front.
    CMD_VOLUME_UP = 0x05,
    CMD_VOLUME_DOWN = 0x06,
    // The user's Bluetooth setting, sent on change and in answer to
    // MSG_HELLO. Off, the ESP32 runs without Bluedroid and light-sleeps
    // until the S3 sends anything; the frame that wakes it is lost.
    CMD_BT_ENABLE = 0x07,
    CMD_BT_DISABLE = 0x08,
    // Bluetooth pairing, driven from the S3's screen. New devices can pair
    // only while the window is open, and only once the user accepts the
    // numeric-comparison code (see pairing_status_t).
    CMD_PAIRING_OPEN = 0x09,
    CMD_PAIRING_CLOSE = 0x0A,
    CMD_PAIRING_ACCEPT = 0x0B,
    CMD_PAIRING_REJECT = 0x0C,
    // Removes every bonded device and drops the current link.
    CMD_FORGET_DEVICES = 0x0D,
} protocol_cmd_t;

typedef struct __attribute__((packed)) {
    uint8_t cmd;  // protocol_cmd_t
} command_t;

typedef enum {
    PAIRING_IDLE = 0,    // not discoverable; only bonded devices connect
    PAIRING_OPEN = 1,    // discoverable, waiting for a phone
    PAIRING_CONFIRM = 2, // code is the 6-digit numeric comparison value
    PAIRING_PASSKEY = 3, // code is a passkey to type on the other device
    PAIRING_BUSY = 4,    // accepted, waiting for the link key
    PAIRING_DONE = 5,    // peer paired; shown briefly, then IDLE
    PAIRING_FAILED = 6,  // shown briefly, then OPEN while the window lasts
} pairing_state_t;

typedef struct __attribute__((packed)) {
    uint8_t state;        // pairing_state_t
    uint8_t seconds_left; // of the pairing window
    uint8_t bonded;       // devices that may reconnect
    uint32_t code;
    char peer[DEVICE_NAME_MAX + 1]; // NUL-terminated; set in DONE
} pairing_status_t;

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
