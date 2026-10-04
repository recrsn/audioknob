#include "uart_link.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "protocol.h"

// Verified on hardware: S3 RX is GPIO38 (fed by ESP32 IO18), S3 TX is GPIO48
// (feeds ESP32 IO23) -- the schematic net names read the other way round.
#define UART_PORT UART_NUM_1
#define UART_TX_GPIO 48
#define UART_RX_GPIO 38

static const char *TAG = "uart_link";

static SemaphoreHandle_t s_state_mutex;
static wireless_state_t s_latest_state;
static bool s_has_state;
static track_info_t s_latest_track;
static bool s_has_track;

static void handle_frame(uint8_t type, const uint8_t *payload, uint8_t len) {
    if (type == MSG_STATE_UPDATE && len == sizeof(wireless_state_t)) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        memcpy(&s_latest_state, payload, sizeof(s_latest_state));
        s_has_state = true;
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

    ESP_LOGW(TAG, "unhandled frame: type=0x%02x len=%d", type, len);
}

static void uart_rx_task(void *arg) {
    (void)arg;
    protocol_parser_t parser;
    protocol_parser_init(&parser);

    uint8_t byte;
    for (;;) {
        if (uart_read_bytes(UART_PORT, &byte, 1, portMAX_DELAY) <= 0) {
            continue;
        }
        switch (protocol_parser_feed(&parser, byte)) {
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

void uart_link_start(void) {
    s_state_mutex = xSemaphoreCreateMutex();

    uart_config_t cfg = {
        .baud_rate = PROTOCOL_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, 256, 256, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, UART_TX_GPIO, UART_RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "inter-MCU UART ready (TX=%d RX=%d)", UART_TX_GPIO, UART_RX_GPIO);
    xTaskCreate(uart_rx_task, "uart_rx", 3072, NULL, 4, NULL);
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

void uart_link_send_command(protocol_cmd_t cmd) {
    command_t command = {.cmd = cmd};
    uint8_t frame[PROTOCOL_MAX_FRAME];
    size_t len = protocol_encode(MSG_COMMAND, &command, sizeof(command), frame);
    // The driver's TX ring serialises concurrent callers.
    uart_write_bytes(UART_PORT, frame, len);
}
