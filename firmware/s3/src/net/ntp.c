#include "net/ntp.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"

#include <time.h>

static const char *TAG = "ntp";
static volatile bool s_synced;

// 2025-01-01T00:00:00Z. The RTC counts from 0 after power-on, so anything
// later was set by a sync before a software reset.
#define RTC_VALID_AFTER 1735689600

static void sync_cb(struct timeval *tv) {
    ESP_LOGI(TAG, "synced: unix_time=%lld", (long long)tv->tv_sec);
    s_synced = true;
}

void ntp_restore_rtc(void) {
    // System time lives in the RTC and survives esp_restart(), panics and
    // deep sleep, so a time from an earlier sync is good until SNTP lands.
    time_t now = time(NULL);
    if (now >= RTC_VALID_AFTER) {
        ESP_LOGI(TAG, "restored from RTC: unix_time=%lld", (long long)now);
        s_synced = true;
    } else {
        ESP_LOGI(TAG, "RTC unset (power-on), waiting for SNTP");
    }
}

void ntp_start(void) {
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.server_from_dhcp = true;
    cfg.renew_servers_after_new_IP = true;
    cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    cfg.sync_cb = sync_cb;
    // The clock is a nicety; never take the device down over it.
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err));
    }
}

bool ntp_is_synced(void) {
    return s_synced;
}
