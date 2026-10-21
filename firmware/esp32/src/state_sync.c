#include "state_sync.h"

#include <stdbool.h>
#include <string.h>

#include "app_state.h"
#include "bt/pairing.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "fw_update.h"
#include "protocol.h"

// Pin directions are the reverse of the schematic net names; see uart_link.c.
#define UART_PORT UART_NUM_1
#define UART_TX_GPIO 18
#define UART_RX_GPIO 23

#define RX_CHUNK 32
// Room for a few whole frames: a firmware chunk is a full-length one.
#define RX_BUF_SIZE 1024
// esp_ota_end()'s image check runs on the RX task.
#define RX_TASK_STACK 6144
// Holds a whole cover-art thumbnail, so streaming one never blocks the
// bt_app task for longer than a frame.
#define TX_BUF_SIZE (16 * 1024)

#define TICK_MS 33
#define RESYNC_TICKS (1000 / TICK_MS)

static const char *TAG = "state_sync";
static state_sync_command_cb_t s_on_command;
static state_sync_name_cb_t s_on_device_name;

static void send_frame(uint8_t type, const void *payload, uint8_t len) {
    uint8_t frame[PROTOCOL_MAX_FRAME];
    size_t frame_len = protocol_encode(type, payload, len, frame);
    uart_write_bytes(UART_PORT, frame, frame_len);
}

void state_sync_send_hello(void) {
    const esp_app_desc_t *app = esp_app_get_description();
    hello_t hello = {0};
    memcpy(hello.app_sha, app->app_elf_sha256, sizeof(hello.app_sha));
    strncpy(hello.version, app->version, FW_VERSION_MAX);
    send_frame(MSG_HELLO, &hello, sizeof(hello));
}

void state_sync_light_sleep(void) {
    uart_wait_tx_done(UART_PORT, portMAX_DELAY);
    // UART wakeup on this chip needs RX on its IO_MUX pin; IO23 isn't, so
    // wake on the start bit's low level instead.
    gpio_wakeup_enable(UART_RX_GPIO, GPIO_INTR_LOW_LEVEL);
    esp_sleep_enable_gpio_wakeup();
    esp_light_sleep_start();
    gpio_wakeup_disable(UART_RX_GPIO);
    state_sync_send_hello();
}

void state_sync_send_track(const track_info_t *track) {
    send_frame(MSG_TRACK_INFO, track, sizeof(*track));
}

void state_sync_send_art_chunk(uint8_t id, uint32_t offset, const uint8_t *data, uint8_t len) {
    art_chunk_t chunk = {.id = id, .offset = offset};
    memcpy(chunk.data, data, len);
    send_frame(MSG_ART_CHUNK, &chunk, ART_CHUNK_HEADER + len);
}

void state_sync_send_art_end(uint8_t id, uint32_t total_len) {
    art_end_t end = {.id = id, .total_len = total_len};
    send_frame(MSG_ART_END, &end, sizeof(end));
}

static void state_sync_task(void *arg) {
    (void)arg;
    wireless_state_t last_sent = {0};
    pairing_status_t last_pairing = {0};
    char last_peer[PEER_NAME_MAX + 1] = "";
    int ticks_since_send = RESYNC_TICKS;  // force an initial send

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));

        wireless_state_t current = app_state_get();
        bool changed = memcmp(&current, &last_sent, sizeof(current)) != 0;
        ticks_since_send++;

        pairing_status_t pairing = bt_pairing_status();
        bool pairing_changed = memcmp(&pairing, &last_pairing, sizeof(pairing)) != 0;

        if (changed || ticks_since_send >= RESYNC_TICKS) {
            send_frame(MSG_STATE_UPDATE, &current, sizeof(current));
            last_sent = current;
        }
        // A lost frame would leave a pairing code unanswered on screen, so
        // this resyncs too.
        if (pairing_changed || ticks_since_send >= RESYNC_TICKS) {
            send_frame(MSG_PAIRING, &pairing, sizeof(pairing));
            last_pairing = pairing;
        }
        char peer[PEER_NAME_MAX + 1];
        app_state_get_peer_name(peer);
        if (strcmp(peer, last_peer) || ticks_since_send >= RESYNC_TICKS) {
            send_frame(MSG_PEER_NAME, peer, (uint8_t)strlen(peer));
            memcpy(last_peer, peer, sizeof(last_peer));
        }
        if (ticks_since_send >= RESYNC_TICKS) {
            ticks_since_send = 0;
        }
    }
}

// Waits up to `wait` for the first byte, then takes whatever else is
// already buffered: at 921600 baud a byte-per-call read spends more time in
// the driver than in the parser.
static int read_available(uint8_t buf[RX_CHUNK], TickType_t wait) {
    if (uart_read_bytes(UART_PORT, buf, 1, wait) <= 0) {
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

// Only the S3's answers to this MCU's hello: they show the link works both
// ways.
static bool is_hello_answer(const protocol_parser_t *frame) {
    if (frame->type == MSG_DEVICE_NAME) {
        return true;
    }
    return frame->type == MSG_COMMAND && frame->len == sizeof(command_t) &&
           (frame->payload[0] == CMD_BT_ENABLE || frame->payload[0] == CMD_BT_DISABLE);
}

static void handle_fw_frame(const protocol_parser_t *frame) {
    fw_ack_t ack;
    bool installed = fw_update_handle(frame->type, frame->payload, frame->len, &ack);
    send_frame(MSG_FW_ACK, &ack, sizeof(ack));
    if (installed) {
        uart_wait_tx_done(UART_PORT, portMAX_DELAY);
        fw_update_restart();
    }
}

static void handle_frame(const protocol_parser_t *frame) {
    if (is_hello_answer(frame)) {
        fw_update_confirm();
    }
    switch (frame->type) {
    case MSG_COMMAND:
        if (frame->len == sizeof(command_t)) {
            s_on_command((protocol_cmd_t)frame->payload[0], 0);
            return;
        }
        if (frame->len == sizeof(command_value_t)) {
            s_on_command((protocol_cmd_t)frame->payload[0], frame->payload[1]);
            return;
        }
        break;
    case MSG_DEVICE_NAME:
        if (frame->len && frame->len <= DEVICE_NAME_MAX) {
            char name[DEVICE_NAME_MAX + 1];
            memcpy(name, frame->payload, frame->len);
            name[frame->len] = '\0';
            s_on_device_name(name);
            return;
        }
        break;
    case MSG_FW_BEGIN:
    case MSG_FW_CHUNK:
    case MSG_FW_END:
        handle_fw_frame(frame);
        return;
    }
    ESP_LOGW(TAG, "unhandled frame: type=0x%02x len=%d", frame->type, frame->len);
}

static void command_rx_task(void *arg) {
    (void)arg;
    protocol_parser_t parser;
    protocol_parser_init(&parser);

    uint8_t buf[RX_CHUNK];
    for (;;) {
        bool in_frame = protocol_parser_in_frame(&parser);
        int n = read_available(buf, in_frame ? pdMS_TO_TICKS(PROTOCOL_FRAME_GAP_MS) : portMAX_DELAY);
        if (!n && in_frame) {
            ESP_LOGW(TAG, "frame type 0x%02x cut short, resyncing", parser.type);
            protocol_parser_init(&parser);
        }
        for (int i = 0; i < n; i++) {
            protocol_feed_result_t result = protocol_parser_feed(&parser, buf[i]);
            if (result == PROTOCOL_FEED_BAD_CRC) {
                ESP_LOGW(TAG, "CRC mismatch, frame type 0x%02x dropped", parser.type);
            } else if (result == PROTOCOL_FEED_FRAME) {
                handle_frame(&parser);
            }
        }
    }
}

void state_sync_start(state_sync_command_cb_t on_command, state_sync_name_cb_t on_device_name) {
    s_on_command = on_command;
    s_on_device_name = on_device_name;

    uart_config_t cfg = {
        .baud_rate = PROTOCOL_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, RX_BUF_SIZE, TX_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_GPIO, UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "inter-MCU UART ready (TX=%d RX=%d)", UART_TX_GPIO, UART_RX_GPIO);
    xTaskCreate(state_sync_task, "state_sync", 3072, NULL, 4, NULL);
    xTaskCreate(command_rx_task, "cmd_rx", RX_TASK_STACK, NULL, 5, NULL);
    state_sync_send_hello();
}
