#pragma once

#include <stdbool.h>
#include <stdint.h>

// Work-dispatch queue that runs Bluedroid callback handlers off the BT stack's
// own task context, so handlers are free to block (I2S writes, UART, etc.)
// without stalling the stack. Every BT/A2DP/AVRCP callback in this codebase
// re-dispatches through here rather than doing work inline.

typedef void (*bt_app_cb_t)(uint16_t event, void *param);
typedef void (*bt_app_free_cb_t)(void *param);
typedef void (*bt_app_copy_cb_t)(void *dest, void *src, int len);

bool bt_app_work_dispatch(bt_app_cb_t cb, uint16_t event, void *params, int param_len,
                           bt_app_copy_cb_t copy_cb, bt_app_free_cb_t free_cb);

void bt_app_task_start_up(void);
