#include "uart_link.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "artwork.h"
#include "esp_heap_caps.h"
#include "protocol.h"

// Verified on hardware: S3 RX is GPIO38 (fed by ESP32 IO18), S3 TX is GPIO48
// (feeds ESP32 IO23) -- the schematic net names read the other way round.
#define UART_PORT UART_NUM_1
#define UART_TX_GPIO 48
#define UART_RX_GPIO 38

// ~45 ms of line time at 921600 baud, so the RX task can be preempted
// briefly without the driver dropping bytes.
#define RX_BUF_SIZE 4096
#define RX_CHUNK 128
// Linked thumbnails are 200x200 baseline JPEGs, typically 10-30 KB.
#define ART_BUF_SIZE (128 * 1024)
// The JPEG decode runs on this task when an image completes.
#define RX_TASK_STACK 6144
// The ESP32 resyncs state every ~1s, so a longer silence means it reset
// or slept (or wasn't up yet) and may have missed what the S3 sent.
#define PEER_LOST_MS 3000

static const char *TAG = "uart_link";

static SemaphoreHandle_t s_state_mutex;
static wireless_state_t s_latest_state;
static bool s_has_state;
static track_info_t s_latest_track;
static bool s_has_track;
static char s_device_name[DEVICE_NAME_MAX + 1];
static int8_t s_bluetooth = -1; // unknown until settings load
static TickType_t s_last_state_tick;
static pairing_status_t s_pairing;
static TickType_t s_last_pairing_tick;
static bool s_has_pairing;
static char s_peer_name[PEER_NAME_MAX + 1];

// Bluetooth cover art being received, in PSRAM. Only the RX task touches it.
static artwork_slot_t *s_art;
static uint8_t *s_art_buf;
static uint8_t s_art_id;
static uint32_t s_art_len;
static bool s_art_valid;

static void handle_art_chunk(const art_chunk_t *chunk, uint8_t len) {
    uint32_t n = len - ART_CHUNK_HEADER;
    if (chunk->offset == 0) {
        s_art_id = chunk->id;
        s_art_len = 0;
        s_art_valid = true;
    }
    if (!s_art_valid || chunk->id != s_art_id) {
        return;
    }
    if (chunk->offset != s_art_len || s_art_len + n > ART_BUF_SIZE) {
        ESP_LOGW(TAG, "cover art chunk at %u, expected %u; dropping image", (unsigned)chunk->offset,
                 (unsigned)s_art_len);
        s_art_valid = false;
        return;
    }
    memcpy(s_art_buf + s_art_len, chunk->data, n);
    s_art_len += n;
}

static void handle_art_end(const art_end_t *end) {
    // Anything short of a complete image clears the art: the previous
    // track's cover would be wrong for this one.
    bool complete = end->total_len && s_art_valid && end->id == s_art_id && end->total_len == s_art_len;
    if (end->total_len && !complete) {
        ESP_LOGW(TAG, "cover art incomplete (%u of %u bytes)", (unsigned)s_art_len, (unsigned)end->total_len);
    }
    if (!complete || !artwork_slot_set(s_art, s_art_buf, s_art_len)) {
        artwork_slot_clear(s_art);
    }
    s_art_valid = false;
}

static void send_frame(uint8_t type, const void *payload, uint8_t len) {
    uint8_t frame[PROTOCOL_MAX_FRAME];
    size_t frame_len = protocol_encode(type, payload, len, frame);
    // The driver's TX ring serialises concurrent callers.
    uart_write_bytes(UART_PORT, frame, frame_len);
}

static void send_device_name(void) {
    char name[DEVICE_NAME_MAX + 1];
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    memcpy(name, s_device_name, sizeof(name));
    xSemaphoreGive(s_state_mutex);
    if (name[0]) {
        send_frame(MSG_DEVICE_NAME, name, strlen(name));
    }
}

static void send_bluetooth(void) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    int8_t on = s_bluetooth;
    xSemaphoreGive(s_state_mutex);
    if (on >= 0) {
        uart_link_send_command(on ? CMD_BT_ENABLE : CMD_BT_DISABLE);
    }
}

static void send_peer_config(void) {
    send_device_name();
    send_bluetooth();
}

static void handle_frame(uint8_t type, const uint8_t *payload, uint8_t len) {
    if (type == MSG_STATE_UPDATE && len == sizeof(wireless_state_t)) {
        TickType_t now = xTaskGetTickCount();
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        memcpy(&s_latest_state, payload, sizeof(s_latest_state));
        bool peer_back = !s_has_state || now - s_last_state_tick > pdMS_TO_TICKS(PEER_LOST_MS);
        s_has_state = true;
        s_last_state_tick = now;
        xSemaphoreGive(s_state_mutex);
        if (peer_back) {
            send_peer_config();
        }
        return;
    }

    if (type == MSG_HELLO) {
        send_peer_config();
        return;
    }

    if (type == MSG_PAIRING && len == sizeof(pairing_status_t)) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        memcpy(&s_pairing, payload, sizeof(s_pairing));
        s_pairing.peer[sizeof(s_pairing.peer) - 1] = '\0';
        s_has_pairing = true;
        s_last_pairing_tick = xTaskGetTickCount();
        xSemaphoreGive(s_state_mutex);
        return;
    }

    if (type == MSG_PEER_NAME && len <= PEER_NAME_MAX) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        memcpy(s_peer_name, payload, len);
        s_peer_name[len] = '\0';
        xSemaphoreGive(s_state_mutex);
        return;
    }

    if (type == MSG_TRACK_INFO && len == sizeof(track_info_t)) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        memcpy(&s_latest_track, payload, sizeof(s_latest_track));
        s_has_track = true;
        xSemaphoreGive(s_state_mutex);
        return;
    }

    if (type == MSG_ART_CHUNK && len > ART_CHUNK_HEADER) {
        handle_art_chunk((const art_chunk_t *)payload, len);
        return;
    }

    if (type == MSG_ART_END && len == sizeof(art_end_t)) {
        handle_art_end((const art_end_t *)payload);
        return;
    }

    ESP_LOGW(TAG, "unhandled frame: type=0x%02x len=%d", type, len);
}

// Blocks for the first byte, then takes whatever else is already buffered:
// at 921600 baud a byte-per-call read spends more time in the driver than
// in the parser.
static int read_available(uint8_t buf[RX_CHUNK]) {
    if (uart_read_bytes(UART_PORT, buf, 1, portMAX_DELAY) <= 0) {
        return 0;
    }
    size_t buffered = 0;
    uart_get_buffered_data_len(UART_PORT, &buffered);
    if (buffered > RX_CHUNK - 1) {
        buffered = RX_CHUNK - 1;
    }
    int more = buffered ? uart_read_bytes(UART_PORT, buf + 1, buffered, 0) : 0;
    return 1 + (more > 0 ? more : 0);
}

static void uart_rx_task(void *arg) {
    (void)arg;
    protocol_parser_t parser;
    protocol_parser_init(&parser);

    uint8_t buf[RX_CHUNK];
    for (;;) {
        int n = read_available(buf);
        for (int i = 0; i < n; i++) {
            switch (protocol_parser_feed(&parser, buf[i])) {
            case PROTOCOL_FEED_FRAME:
                handle_frame(parser.type, parser.payload, parser.len);
                break;
            case PROTOCOL_FEED_BAD_CRC:
                ESP_LOGW(TAG, "CRC mismatch, frame type 0x%02x dropped", parser.type);
                break;
            case PROTOCOL_FEED_PENDING:
                break;
            }
        }
    }
}

void uart_link_start(void) {
    s_state_mutex = xSemaphoreCreateMutex();
    s_art = artwork_slot_create();
    s_art_buf = heap_caps_malloc(ART_BUF_SIZE, MALLOC_CAP_SPIRAM);
    ESP_ERROR_CHECK(s_art && s_art_buf ? ESP_OK : ESP_ERR_NO_MEM);

    uart_config_t cfg = {
        .baud_rate = PROTOCOL_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, RX_BUF_SIZE, 256, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_GPIO, UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "inter-MCU UART ready (TX=%d RX=%d)", UART_TX_GPIO, UART_RX_GPIO);
    xTaskCreate(uart_rx_task, "uart_rx", RX_TASK_STACK, NULL, 4, NULL);
}

bool uart_link_get_latest(wireless_state_t *out) {
    bool has_state;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    has_state = s_has_state;
    if (has_state) {
        memcpy(out, &s_latest_state, sizeof(*out));
    }
    xSemaphoreGive(s_state_mutex);
    return has_state;
}

bool uart_link_get_latest_track(track_info_t *out) {
    bool has_track;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    has_track = s_has_track;
    if (has_track) {
        memcpy(out, &s_latest_track, sizeof(*out));
    }
    xSemaphoreGive(s_state_mutex);
    return has_track;
}

void uart_link_get_peer_name(char *out, size_t len) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    snprintf(out, len, "%s", s_peer_name);
    xSemaphoreGive(s_state_mutex);
}

bool uart_link_get_pairing(pairing_status_t *out) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    // Resent every second; a stale code must not stay up for an answer.
    bool fresh = s_has_pairing && xTaskGetTickCount() - s_last_pairing_tick <= pdMS_TO_TICKS(PEER_LOST_MS);
    if (fresh) {
        *out = s_pairing;
    }
    xSemaphoreGive(s_state_mutex);
    return fresh;
}

const uint16_t *uart_link_get_artwork(uint32_t *version) {
    return artwork_slot_get(s_art, version);
}

void uart_link_send_command(protocol_cmd_t cmd) {
    command_t command = {.cmd = cmd};
    send_frame(MSG_COMMAND, &command, sizeof(command));
}

void uart_link_set_device_name(const char *name) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    snprintf(s_device_name, sizeof(s_device_name), "%s", name);
    xSemaphoreGive(s_state_mutex);
    send_device_name();
}

void uart_link_set_bluetooth(bool enabled) {
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_bluetooth = enabled;
    xSemaphoreGive(s_state_mutex);
    send_bluetooth();
}
