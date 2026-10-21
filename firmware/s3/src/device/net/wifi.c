#include "wifi.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "network_jobs.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "device_name.h"
#include "nvs_flash.h"
#include "wifi_portal.h"

#define NVS_NAMESPACE "wifi"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASSWORD "password"

#define SETUP_AP_PASSWORD_LEN 8
#define SETUP_AP_MAX_CLIENTS 4

// Saved credentials get this long after boot before setup mode takes over.
#define CONNECT_TIMEOUT_US (60 * 1000 * 1000)
// A credential attempt from the portal that hasn't resolved by then failed.
#define ATTEMPT_TIMEOUT_US (20 * 1000 * 1000)
// In setup mode, how often to retry saved credentials in the background
// (e.g. the router was just slower to boot than we were).
#define RETRY_SAVED_US (30 * 1000 * 1000)
// Long enough for the portal's status page to show the result.
#define RESTART_DELAY_US (3 * 1000 * 1000)

#define MAX_SCAN 20

static const char *TAG = "wifi";

static wifi_got_ip_cb_t s_on_got_ip;
static esp_netif_t *s_sta_netif;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_setup_task;

static volatile bool s_setup_mode;
static bool s_has_saved;
static wifi_config_t s_saved_cfg;

static wifi_setup_info_t s_setup;
static bool s_attempt_pending;
static char s_attempt_ssid[33];
static char s_attempt_password[65];

static wifi_network_t s_scan[MAX_SCAN];
static size_t s_scan_count;

static esp_timer_handle_t s_connect_timer, s_attempt_timer, s_retry_timer, s_restart_timer;

// --- Credentials -----------------------------------------------------------

static bool load_credentials(wifi_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t ssid_len = sizeof(cfg->sta.ssid);
        size_t pass_len = sizeof(cfg->sta.password);
        bool ok = nvs_get_str(nvs, NVS_KEY_SSID, (char *)cfg->sta.ssid, &ssid_len) == ESP_OK &&
                  nvs_get_str(nvs, NVS_KEY_PASSWORD, (char *)cfg->sta.password, &pass_len) == ESP_OK;
        nvs_close(nvs);
        if (ok && cfg->sta.ssid[0]) {
            return true;
        }
        memset(cfg, 0, sizeof(*cfg));
    }

    // Build-time credentials remain a convenience for development.
    if (CONFIG_AK_WIFI_SSID[0]) {
        snprintf((char *)cfg->sta.ssid, sizeof(cfg->sta.ssid), "%s", CONFIG_AK_WIFI_SSID);
        snprintf((char *)cfg->sta.password, sizeof(cfg->sta.password), "%s", CONFIG_AK_WIFI_PASSWORD);
        return true;
    }
    return false;
}

static void save_credentials(const char *ssid, const char *password) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "cannot open NVS to save credentials");
        return;
    }
    nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    nvs_set_str(nvs, NVS_KEY_PASSWORD, password);
    nvs_commit(nvs);
    nvs_close(nvs);
}

// --- Setup-mode state ------------------------------------------------------

static void set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_status(const char *fmt, ...) {
    va_list args;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    va_start(args, fmt);
    vsnprintf(s_setup.status, sizeof(s_setup.status), fmt, args);
    va_end(args);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "setup: %s", s_setup.status);
}

static void restart_soon(void) {
    esp_timer_start_once(s_restart_timer, RESTART_DELAY_US);
}

static void attempt_failed(const char *why) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_pending = s_attempt_pending;
    s_attempt_pending = false;
    xSemaphoreGive(s_lock);
    if (!was_pending) {
        return;
    }

    esp_timer_stop(s_attempt_timer);
    set_status("%s \"%s\". Try again.", why, s_attempt_ssid);
    esp_wifi_disconnect();
    if (s_has_saved) {
        esp_wifi_set_config(WIFI_IF_STA, &s_saved_cfg);
    }
}

static const char *describe_disconnect(uint8_t reason) {
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "Wrong password for";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "Couldn't find";
    default:
        return "Couldn't connect to";
    }
}

// --- Portal ops (called from the HTTP server task) -------------------------

static void scan_networks(void) {
    wifi_scan_config_t scan_cfg = {0};
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        return;  // e.g. a connection attempt is in progress; keep the old list
    }

    uint16_t n = MAX_SCAN;
    wifi_ap_record_t *records = calloc(MAX_SCAN, sizeof(wifi_ap_record_t));
    if (!records || esp_wifi_scan_get_ap_records(&n, records) != ESP_OK) {
        free(records);
        return;
    }

    // Records arrive strongest first; keep the first entry per SSID.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_scan_count = 0;
    for (uint16_t i = 0; i < n; i++) {
        const char *ssid = (const char *)records[i].ssid;
        bool duplicate = !ssid[0];
        for (size_t j = 0; j < s_scan_count && !duplicate; j++) {
            duplicate = !strcmp(s_scan[j].ssid, ssid);
        }
        if (!duplicate) {
            wifi_network_t *net = &s_scan[s_scan_count++];
            snprintf(net->ssid, sizeof(net->ssid), "%s", ssid);
            net->rssi = records[i].rssi;
            net->secure = records[i].authmode != WIFI_AUTH_OPEN;
        }
    }
    xSemaphoreGive(s_lock);
    free(records);
}

static size_t portal_scan(wifi_network_t *out, size_t max, bool refresh) {
    if (refresh) {
        scan_networks();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_scan_count < max ? s_scan_count : max;
    memcpy(out, s_scan, n * sizeof(*out));
    xSemaphoreGive(s_lock);
    return n;
}

static void portal_submit(const char *ssid, const char *password) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(s_attempt_ssid, sizeof(s_attempt_ssid), "%s", ssid);
    snprintf(s_attempt_password, sizeof(s_attempt_password), "%s", password);
    s_attempt_pending = true;
    xSemaphoreGive(s_lock);
    set_status("Connecting to \"%s\"...", ssid);

    wifi_config_t cfg = {0};
    snprintf((char *)cfg.sta.ssid, sizeof(cfg.sta.ssid), "%s", ssid);
    snprintf((char *)cfg.sta.password, sizeof(cfg.sta.password), "%s", password);
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();
    esp_timer_stop(s_attempt_timer);
    esp_timer_start_once(s_attempt_timer, ATTEMPT_TIMEOUT_US);
}

static void portal_status(char *out, size_t len) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(out, len, "%s", s_setup.status);
    xSemaphoreGive(s_lock);
}

static const wifi_portal_ops_t s_portal_ops = {
    .scan = portal_scan,
    .submit = portal_submit,
    .status = portal_status,
};

// --- Setup mode ------------------------------------------------------------

// WIFI: join codes backslash-escape \ ; , : and ".
static void qr_escape(const char *in, char *out, size_t len) {
    size_t n = 0;
    for (; *in && n + 2 < len; in++) {
        if (strchr("\\;,:\"", *in)) {
            out[n++] = '\\';
        }
        out[n++] = *in;
    }
    out[n] = '\0';
}

// The hotspot is named after the device (an SSID holds DEVICE_NAME_MAX
// bytes), so the network to join matches what the screen shows.
static void make_setup_credentials(void) {
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";  // no 0/o, 1/l/i
    char ssid_escaped[2 * DEVICE_NAME_MAX + 1];

    xSemaphoreTake(s_lock, portMAX_DELAY);
    device_name_get(s_setup.ap_ssid, sizeof(s_setup.ap_ssid));
    for (int i = 0; i < SETUP_AP_PASSWORD_LEN; i++) {
        s_setup.ap_password[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
    }
    s_setup.ap_password[SETUP_AP_PASSWORD_LEN] = '\0';
    qr_escape(s_setup.ap_ssid, ssid_escaped, sizeof(ssid_escaped));
    snprintf(s_setup.qr_payload, sizeof(s_setup.qr_payload), "WIFI:T:WPA;S:%s;P:%s;;", ssid_escaped,
             s_setup.ap_password);
    xSemaphoreGive(s_lock);
}

static void enter_setup_mode(void) {
    ESP_LOGW(TAG, "entering WiFi setup mode");
    s_setup_mode = true;
    esp_wifi_disconnect();

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    make_setup_credentials();

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = SETUP_AP_MAX_CLIENTS,
        },
    };
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", s_setup.ap_ssid);
    ap_cfg.ap.ssid_len = strlen(s_setup.ap_ssid);
    snprintf((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), "%s", s_setup.ap_password);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    // Scan before any phone joins: a scan briefly takes the radio off the
    // AP's channel.
    set_status("Scanning for networks...");
    scan_networks();

    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(ap_netif, &ip);
    wifi_portal_start(ip.ip.addr, &s_portal_ops);

    set_status("Scan with your phone camera");
    if (s_has_saved) {
        esp_timer_start_periodic(s_retry_timer, RETRY_SAVED_US);
    }
}

static void setup_task(void *arg) {
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    enter_setup_mode();
    vTaskDelete(NULL);
}

// --- Timers ----------------------------------------------------------------

static void connect_timeout_cb(void *arg) {
    (void)arg;
    ESP_LOGW(TAG, "no connection within %d s", CONNECT_TIMEOUT_US / 1000000);
    xTaskNotifyGive(s_setup_task);
}

static void attempt_timeout_cb(void *arg) {
    (void)arg;
    attempt_failed("Couldn't connect to");
}

static void retry_saved_cb(void *arg) {
    (void)arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = s_attempt_pending;
    xSemaphoreGive(s_lock);
    if (!busy) {
        esp_wifi_connect();
    }
}

static void restart_cb(void *arg) {
    (void)arg;
    esp_restart();
}

// --- Events ----------------------------------------------------------------

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_has_saved) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        network_jobs_set_network(false);
        wifi_event_sta_disconnected_t *event = data;
        if (!s_setup_mode) {
            ESP_LOGW(TAG, "disconnected (reason %d), retrying", event->reason);
            esp_wifi_connect();
        } else {
            attempt_failed(describe_disconnect(event->reason));
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&event->ip_info.ip));

        if (!s_setup_mode) {
            esp_timer_stop(s_connect_timer);
            uint8_t mac[6];
            esp_wifi_get_mac(WIFI_IF_STA, mac);
            s_on_got_ip(mac);
            return;
        }

        // Setup mode: either the portal's credentials or the saved ones just
        // worked. Restart into normal mode rather than tearing down the
        // portal piecemeal.
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool from_portal = s_attempt_pending;
        s_attempt_pending = false;
        xSemaphoreGive(s_lock);
        esp_timer_stop(s_attempt_timer);
        esp_timer_stop(s_retry_timer);

        if (from_portal) {
            save_credentials(s_attempt_ssid, s_attempt_password);
            set_status("Connected to \"%s\". Restarting...", s_attempt_ssid);
        } else {
            set_status("Reconnected. Restarting...");
        }
        restart_soon();
    }
}

static void nvs_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static esp_timer_handle_t make_timer(esp_timer_cb_t cb, const char *name) {
    const esp_timer_create_args_t args = {.callback = cb, .name = name};
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    return timer;
}

void wifi_start(wifi_got_ip_cb_t on_got_ip) {
    s_on_got_ip = on_got_ip;
    s_lock = xSemaphoreCreateMutex();
    s_connect_timer = make_timer(connect_timeout_cb, "wifi_connect");
    s_attempt_timer = make_timer(attempt_timeout_cb, "wifi_attempt");
    s_retry_timer = make_timer(retry_saved_cb, "wifi_retry");
    s_restart_timer = make_timer(restart_cb, "wifi_restart");
    xTaskCreate(setup_task, "wifi_setup", 4096, NULL, 5, &s_setup_task);

    nvs_init();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    // Credentials live in our own NVS namespace, not the driver's copy.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL));

    s_has_saved = load_credentials(&s_saved_cfg);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (s_has_saved) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &s_saved_cfg));
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    // Modem sleep delays inbound packets by up to a beacon interval, which
    // shows up as AirPlay resend storms and dropouts.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if (s_has_saved) {
        ESP_LOGI(TAG, "connecting to \"%s\"", s_saved_cfg.sta.ssid);
        esp_timer_start_once(s_connect_timer, CONNECT_TIMEOUT_US);
    } else {
        xTaskNotifyGive(s_setup_task);
    }
}

bool wifi_get_station(char *ssid, size_t ssid_len, char *ip, size_t ip_len) {
    wifi_ap_record_t ap;
    esp_netif_ip_info_t info;
    if (s_setup_mode || !s_sta_netif || esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
        esp_netif_get_ip_info(s_sta_netif, &info) != ESP_OK || !info.ip.addr) {
        return false;
    }
    snprintf(ssid, ssid_len, "%s", (const char *)ap.ssid);
    esp_ip4addr_ntoa(&info.ip, ip, ip_len);
    return true;
}

static void forget_task(void *arg) {
    (void)arg;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_all(nvs);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    ESP_LOGI(TAG, "credentials erased; restarting into setup");
    esp_restart();
}

// Its caller may be the LVGL task, whose PSRAM stack can't write flash.
void wifi_forget(void) {
    if (xTaskCreate(forget_task, "wifi_forget", 3072, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory to forget credentials");
    }
}

bool wifi_get_setup_info(wifi_setup_info_t *out) {
    if (!s_setup_mode || !s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_setup;
    xSemaphoreGive(s_lock);
    return true;
}
