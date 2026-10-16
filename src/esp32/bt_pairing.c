#include "bt_pairing.h"

#include <stdlib.h>
#include <string.h>

#include "bt_app_core.h"
#include "bt_gap.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define WINDOW_S 120
// How long DONE/FAILED stay on screen.
#define RESULT_HOLD_S 3
#define TICK_US (1000 * 1000)
// Time left for the link key to arrive once the user accepts.
#define ACCEPT_GRACE_S 15
// bt_app work events past protocol_cmd_t's byte range.
#define WORK_TICK    0x100
#define WORK_PUBLISH 0x101

static const char *TAG = "bt_pairing";

// Owned by the bt_app task; publish() copies it out for state_sync.
static pairing_status_t s_status;
static pairing_status_t s_shared;
static portMUX_TYPE s_shared_lock = portMUX_INITIALIZER_UNLOCKED;

// The device being paired in CONFIRM, PASSKEY and BUSY.
static esp_bd_addr_t s_peer;
static bool s_has_peer;
// s_peer bonded Just Works in this window and owes us the numeric
// comparison: the stack re-pairs for it as soon as a profile asks for MITM.
static bool s_weak_bond;
static uint8_t s_hold_s;
static esp_timer_handle_t s_tick;

static void publish(void) {
    int bonded = esp_bt_gap_get_bond_device_num();
    s_status.bonded = bonded > 0 ? (uint8_t)bonded : 0;
    portENTER_CRITICAL(&s_shared_lock);
    s_shared = s_status;
    portEXIT_CRITICAL(&s_shared_lock);
}

static void set_state(pairing_state_t state) {
    s_status.state = state;
    if (state != PAIRING_CONFIRM && state != PAIRING_PASSKEY) {
        s_status.code = 0;
    }
}

static void tick_start(void) {
    if (!esp_timer_is_active(s_tick)) {
        esp_timer_start_periodic(s_tick, TICK_US);
    }
}

static void window_open(void) {
    s_status.seconds_left = WINDOW_S;
    s_status.peer[0] = '\0';
    s_hold_s = 0;
    set_state(PAIRING_OPEN);
    bt_gap_set_discoverable(true);
    tick_start();
    ESP_LOGI(TAG, "pairing window open for %d s", WINDOW_S);
}

// Ends the window, turning down a code the user hasn't answered yet. `next`
// is IDLE, or DONE to show the result for a moment first.
static void window_close(pairing_state_t next) {
    if (s_has_peer && s_status.state == PAIRING_CONFIRM) {
        esp_bt_gap_ssp_confirm_reply(s_peer, false);
    }
    if (s_weak_bond) {
        esp_bt_gap_remove_bond_device(s_peer);
        s_weak_bond = false;
    }
    s_has_peer = false;
    s_status.seconds_left = 0;
    set_state(next);
    bt_gap_set_discoverable(false);
    s_hold_s = next == PAIRING_IDLE ? 0 : RESULT_HOLD_S;
    if (next == PAIRING_IDLE) {
        esp_timer_stop(s_tick);
    } else {
        tick_start();
    }
}

static void on_tick(void) {
    if (s_hold_s && --s_hold_s == 0) {
        set_state(s_status.seconds_left ? PAIRING_OPEN : PAIRING_IDLE);
    }
    if (s_status.seconds_left && --s_status.seconds_left == 0) {
        ESP_LOGI(TAG, "pairing window closed");
        window_close(PAIRING_IDLE);
    }
    if (s_status.state == PAIRING_IDLE && !s_hold_s) {
        esp_timer_stop(s_tick);
    }
}

static void forget_all(void) {
    int n = esp_bt_gap_get_bond_device_num();
    if (n <= 0) {
        return;
    }
    esp_bd_addr_t *list = malloc(n * sizeof(esp_bd_addr_t));
    if (!list) {
        return;
    }
    if (esp_bt_gap_get_bond_device_list(&n, list) == ESP_OK) {
        // Also drops the link to any of them that's connected.
        for (int i = 0; i < n; i++) {
            esp_bt_gap_remove_bond_device(list[i]);
        }
        ESP_LOGI(TAG, "forgetting %d device(s)", n);
    }
    free(list);
}

static void on_command(protocol_cmd_t cmd) {
    switch (cmd) {
    case CMD_PAIRING_OPEN:
        if (s_status.state == PAIRING_IDLE || s_status.state == PAIRING_DONE) {
            window_open();
        }
        break;
    case CMD_PAIRING_CLOSE:
        window_close(PAIRING_IDLE);
        break;
    case CMD_PAIRING_ACCEPT:
        if (s_status.state == PAIRING_CONFIRM && s_has_peer) {
            esp_bt_gap_ssp_confirm_reply(s_peer, true);
            set_state(PAIRING_BUSY);
            if (s_status.seconds_left < ACCEPT_GRACE_S) {
                s_status.seconds_left = ACCEPT_GRACE_S;
            }
        }
        break;
    case CMD_PAIRING_REJECT:
        // A code the user doesn't recognise means someone else is pairing;
        // don't leave the window open for another try.
        if (s_status.state == PAIRING_CONFIRM) {
            ESP_LOGW(TAG, "pairing code rejected on screen");
            window_close(PAIRING_IDLE);
        }
        break;
    case CMD_FORGET_DEVICES:
        forget_all();
        break;
    default:
        break;
    }
}

// Keeps a name that's cut short from ending mid UTF-8 sequence.
static void copy_name(char *dst, size_t len, const uint8_t *src) {
    size_t n = strnlen((const char *)src, len - 1);
    if (n == len - 1) {
        while (n && (src[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool is_peer(const uint8_t *bda) {
    return s_has_peer && !memcmp(bda, s_peer, ESP_BD_ADDR_LEN);
}

static bool is_authenticated(esp_bt_link_key_type_t type) {
    return type == ESP_BT_LINK_KEY_AUTHED_COMB_P192 || type == ESP_BT_LINK_KEY_AUTHED_COMB_P256;
}

static void on_paired(esp_bt_gap_cb_param_t *param) {
    // OPEN: Just Works, which Bluedroid completes without asking us.
    bool expected = s_status.seconds_left &&
                    ((s_status.state == PAIRING_OPEN && (!s_has_peer || is_peer(param->auth_cmpl.bda))) ||
                     ((s_status.state == PAIRING_BUSY || s_status.state == PAIRING_PASSKEY) &&
                      is_peer(param->auth_cmpl.bda)));
    if (!expected) {
        // A new device, or one spoofing a bonded address, pairing Just
        // Works with no window open.
        ESP_LOGW(TAG, "removing bond made outside the pairing window: " ESP_BD_ADDR_STR,
                 ESP_BD_ADDR_HEX(param->auth_cmpl.bda));
        esp_bt_gap_remove_bond_device(param->auth_cmpl.bda);
        return;
    }
    if (!is_authenticated(param->auth_cmpl.lk_type)) {
        // As acceptor Bluedroid never asks for MITM, so a phone that doesn't
        // either (iPhones don't) bonds Just Works first. A2DP and AVRCP
        // need MITM, so the numeric comparison follows right away; that's
        // what the user confirms.
        ESP_LOGI(TAG, "unauthenticated key (type %d), waiting for the code", param->auth_cmpl.lk_type);
        memcpy(s_peer, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
        s_has_peer = true;
        s_weak_bond = true;
        if (s_status.seconds_left < ACCEPT_GRACE_S) {
            s_status.seconds_left = ACCEPT_GRACE_S;
        }
        return;
    }
    s_weak_bond = false;
    ESP_LOGI(TAG, "paired with %s, link key type %d", param->auth_cmpl.device_name, param->auth_cmpl.lk_type);
    copy_name(s_status.peer, sizeof(s_status.peer), param->auth_cmpl.device_name);
    window_close(PAIRING_DONE);
}

static void start_code(const uint8_t *bda, uint32_t code, pairing_state_t state) {
    memcpy(s_peer, bda, ESP_BD_ADDR_LEN);
    s_has_peer = true;
    s_status.code = code;
    set_state(state);
}

static void on_gap_event(uint16_t event, void *arg) {
    esp_bt_gap_cb_param_t *param = arg;
    switch (event) {
    case ESP_BT_GAP_CFM_REQ_EVT:
        if (s_status.state != PAIRING_OPEN || (s_has_peer && !is_peer(param->cfm_req.bda))) {
            ESP_LOGW(TAG, "pairing request outside the window, refused");
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, false);
            break;
        }
        start_code(param->cfm_req.bda, param->cfm_req.num_val, PAIRING_CONFIRM);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        // The other side has only a keyboard. Outside the window there's no
        // way to refuse here; on_paired() removes the bond if it completes.
        if (s_status.state == PAIRING_OPEN) {
            start_code(param->key_notif.bda, param->key_notif.passkey, PAIRING_PASSKEY);
        }
        break;
    case ESP_BT_GAP_KEY_REQ_EVT:
        esp_bt_gap_ssp_passkey_reply(param->key_req.bda, false, 0);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT:
        // Legacy (pre-2.1) pairing: a guessable PIN and no protection
        // against eavesdropping.
        ESP_LOGW(TAG, "legacy PIN pairing refused");
        esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, NULL);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        // Bluedroid reports success only with a new link key, i.e. a pairing.
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            on_paired(param);
        } else if (is_peer(param->auth_cmpl.bda)) {
            ESP_LOGW(TAG, "pairing failed, status %d", param->auth_cmpl.stat);
            if (s_weak_bond) {
                esp_bt_gap_remove_bond_device(s_peer);
                s_weak_bond = false;
            }
            s_has_peer = false;
            set_state(PAIRING_FAILED);
            s_hold_s = RESULT_HOLD_S;
            tick_start();
        }
        break;
    default:
        break;
    }
    publish();
}

static void on_work(uint16_t event, void *arg) {
    (void)arg;
    if (event == WORK_TICK) {
        on_tick();
    } else if (event != WORK_PUBLISH) {
        on_command((protocol_cmd_t)event);
    }
    publish();
}

static void tick_cb(void *arg) {
    (void)arg;
    bt_app_work_dispatch(on_work, WORK_TICK, NULL, 0, NULL, NULL);
}

void bt_pairing_init(void) {
    const esp_timer_create_args_t args = {.callback = tick_cb, .name = "bt_pairing"};
    ESP_ERROR_CHECK(esp_timer_create(&args, &s_tick));
    bt_gap_set_discoverable(false);
    bt_app_work_dispatch(on_work, WORK_PUBLISH, NULL, 0, NULL, NULL);
}

void bt_pairing_gap_event(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    bt_app_work_dispatch(on_gap_event, event, param, sizeof(*param), NULL, NULL);
}

void bt_pairing_command(protocol_cmd_t cmd) {
    if (s_tick) {
        bt_app_work_dispatch(on_work, cmd, NULL, 0, NULL, NULL);
    }
}

pairing_status_t bt_pairing_status(void) {
    portENTER_CRITICAL(&s_shared_lock);
    pairing_status_t copy = s_shared;
    portEXIT_CRITICAL(&s_shared_lock);
    return copy;
}
