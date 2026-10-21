#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
#define portENTER_CRITICAL_ISR(x) ((void)(x))
#define portEXIT_CRITICAL_ISR(x) ((void)(x))
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_INTR_ANYEDGE 1
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERROR_CHECK(x) assert((x) == 0)
typedef int esp_err_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, intr_type; } gpio_config_t;
static unsigned test_levels = 3;
static uint32_t test_ms = 1000;
static int gpio_get_level(int pin) { return (test_levels >> (pin == 8 ? 0 : 1)) & 1; }
static int64_t esp_timer_get_time(void) { return (int64_t)test_ms * 1000; }
static int gpio_config(const gpio_config_t *cfg) { (void)cfg; return 0; }
static int gpio_install_isr_service(int flags) { (void)flags; return 0; }
static int gpio_isr_handler_add(int pin, void (*fn)(void *), void *arg) {
    (void)pin; (void)fn; (void)arg; return 0;
}
