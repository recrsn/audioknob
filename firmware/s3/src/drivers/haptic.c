#include "haptic.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"

#define DRV2605_ADDR   0x5A
#define I2C_HZ         400000
#define I2C_TIMEOUT_MS 50

#define REG_STATUS   0x00
#define REG_MODE     0x01
#define REG_LIBRARY  0x03
#define REG_WAVESEQ1 0x04
#define REG_GO       0x0C
#define REG_FEEDBACK 0x1A

#define STATUS_DEVICE_ID(s) ((s) >> 5)
#define DEVICE_ID_DRV2605   3
#define DEVICE_ID_DRV2605L  7
// Not a TI ID: this board's part (a DRV2605L on the schematic) reports it,
// most likely a register-compatible second source.
#define DEVICE_ID_COMPAT    5

#define MODE_INTERNAL_TRIGGER 0x00 // also clears STANDBY
#define LIBRARY_LRA           6
#define FEEDBACK_LRA          0x80

// ROM library effect numbers (DRV2605 datasheet, table 11.2); a sequencer
// byte with bit 7 set is a wait of (value & 0x7F) * 10 ms.
#define EFFECT_STRONG_CLICK 1
#define EFFECT_DOUBLE_CLICK 10
#define EFFECT_SHARP_TICK   24
#define EFFECT_BUZZ         47
#define SEQ_WAIT(ms)    (0x80 | ((ms) / 10))
#define ALARM_PERIOD_MS 1000

static const char *TAG = "haptic";

static i2c_master_dev_handle_t s_dev;
static esp_timer_handle_t s_alarm_timer;

static esp_err_t reg_write(uint8_t reg, uint8_t value) {
    uint8_t buf[] = {reg, value};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), I2C_TIMEOUT_MS);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *value) {
    return i2c_master_transmit_receive(s_dev, &reg, 1, value, 1, I2C_TIMEOUT_MS);
}

// seq starts with REG_WAVESEQ1 and ends with a 0 terminator; registers
// auto-increment, so the whole sequence goes in one write.
static void play_sequence(const uint8_t *seq, size_t len) {
    if (i2c_master_transmit(s_dev, seq, len, I2C_TIMEOUT_MS) != ESP_OK || reg_write(REG_GO, 1) != ESP_OK) {
        ESP_LOGW(TAG, "sequence write failed");
    }
}

// Buzz, pause, buzz: reads as an alarm rather than a single notification.
static void alarm_fire(void *arg) {
    (void)arg;
    static const uint8_t seq[] = {REG_WAVESEQ1, EFFECT_BUZZ, SEQ_WAIT(100), EFFECT_BUZZ, 0};
    play_sequence(seq, sizeof(seq));
}

static bool configure(void) {
    uint8_t status, feedback;
    if (reg_read(REG_STATUS, &status) != ESP_OK) {
        return false;
    }
    uint8_t id = STATUS_DEVICE_ID(status);
    if (id != DEVICE_ID_DRV2605 && id != DEVICE_ID_DRV2605L && id != DEVICE_ID_COMPAT) {
        ESP_LOGW(TAG, "unexpected device id %u at 0x%02x", id, DRV2605_ADDR);
        return false;
    }
    ESP_LOGI(TAG, "device id %u", id);
    // The board's actuator is an LRA: switch the driver from its ERM reset
    // default and use the LRA effect library. Closed-loop resonance tracking
    // works from reset values, so no auto-calibration pass.
    return reg_write(REG_MODE, MODE_INTERNAL_TRIGGER) == ESP_OK &&
           reg_read(REG_FEEDBACK, &feedback) == ESP_OK &&
           reg_write(REG_FEEDBACK, feedback | FEEDBACK_LRA) == ESP_OK &&
           reg_write(REG_LIBRARY, LIBRARY_LRA) == ESP_OK;
}

void haptic_init(i2c_master_bus_handle_t bus) {
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DRV2605_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        return;
    }
    if (!configure()) {
        ESP_LOGW(TAG, "DRV2605 not found; haptics disabled");
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return;
    }

    const esp_timer_create_args_t timer_args = {.callback = alarm_fire, .name = "haptic_alarm"};
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_alarm_timer));
    ESP_LOGI(TAG, "DRV2605 ready (LRA)");
}

void haptic_play(haptic_effect_t effect) {
    static const uint8_t effects[] = {
        [HAPTIC_TICK] = EFFECT_SHARP_TICK,
        [HAPTIC_CLICK] = EFFECT_STRONG_CLICK,
        [HAPTIC_LONG] = EFFECT_DOUBLE_CLICK,
    };
    if (!s_alarm_timer || esp_timer_is_active(s_alarm_timer)) {
        return;
    }
    const uint8_t seq[] = {REG_WAVESEQ1, effects[effect], 0};
    play_sequence(seq, sizeof(seq));
}

void haptic_alarm_start(void) {
    if (!s_alarm_timer || esp_timer_is_active(s_alarm_timer)) {
        return;
    }
    alarm_fire(NULL);
    esp_timer_start_periodic(s_alarm_timer, ALARM_PERIOD_MS * 1000);
}

void haptic_alarm_stop(void) {
    if (s_alarm_timer && esp_timer_is_active(s_alarm_timer)) {
        esp_timer_stop(s_alarm_timer);
    }
}
