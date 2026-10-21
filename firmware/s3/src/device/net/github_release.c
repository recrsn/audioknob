#include "github_release.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "https_download.h"
#include "ntp.h"
#include "release_manifest.h"
#include "sdkconfig.h"
#include "uart_link.h"
#include "wifi.h"

#define RELEASE_JSON_CAP (64 * 1024)
#define DAY_US (24LL * 60 * 60 * 1000000)
#define RETRY_US (60LL * 60 * 1000000)

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static github_release_status_t s_status;
static release_manifest_t s_manifest;

static void refresh_available(const hello_t *bluetooth) {
    s_status.s3_available =
        s_manifest.s3.size && release_version_newer(s_manifest.s3.version, esp_app_get_description()->version);
    s_status.bluetooth_available = s_manifest.bluetooth.size && bluetooth->version[0] &&
                                   release_version_newer(s_manifest.bluetooth.version, bluetooth->version);
    snprintf(s_status.s3_version, sizeof(s_status.s3_version), "%s", s_manifest.s3.version);
    snprintf(s_status.bluetooth_version, sizeof(s_status.bluetooth_version), "%s", s_manifest.bluetooth.version);
    s_status.target = s_status.s3_available ? OTA_TARGET_S3 : OTA_TARGET_ESP32;
    if (s_status.notice == RELEASE_AVAILABLE && !s_status.s3_available && !s_status.bluetooth_available) {
        s_status.notice = RELEASE_QUIET;
    }
}

void github_release_get_status(github_release_status_t *out) {
    hello_t bluetooth;
    uart_link_get_esp32_firmware(&bluetooth);
    taskENTER_CRITICAL(&s_lock);
    refresh_available(&bluetooth);
    *out = s_status;
    taskEXIT_CRITICAL(&s_lock);
}

static void check_release(void) {
    const char *error = NULL;
    release_manifest_t *manifest = calloc(1, sizeof(*manifest));
    char *body = heap_caps_malloc(RELEASE_JSON_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char url[256];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest",
             CONFIG_AK_UPDATE_REPOSITORY);
    int status = 0;
    esp_http_client_handle_t client = NULL;
    if (!manifest || !body) {
        error = "Out of memory";
    } else if (!(client = https_download_open(url, &status))) {
        error = status == 404                    ? "No published release yet"
                : status == 403 || status == 429 ? "GitHub rate limit; try again later"
                                                 : "Couldn't check GitHub (check WiFi/time)";
    } else {
        size_t total = 0;
        int n = 0;
        while (total < RELEASE_JSON_CAP - 1 &&
               (n = esp_http_client_read(client, body + total, RELEASE_JSON_CAP - 1 - total)) > 0) {
            total += n;
        }
        body[total] = '\0';
        if (n < 0 || !esp_http_client_is_complete_data_received(client)) {
            error = "Incomplete or oversized release response";
        } else if (!release_manifest_parse(body, CONFIG_AK_UPDATE_REPOSITORY, manifest)) {
            error = "Release has no verified compatible assets";
        }
    }
    if (client) {
        esp_http_client_cleanup(client);
    }
    free(body);
    hello_t bluetooth;
    uart_link_get_esp32_firmware(&bluetooth);
    taskENTER_CRITICAL(&s_lock);
    bool manual = s_status.notice == RELEASE_CHECKING;
    s_status.checking = false;
    s_status.checked = true;
    snprintf(s_status.error, sizeof(s_status.error), "%s", error ? error : "");
    if (!error) {
        // Dismissal lasts for this release and boot, even across daily checks.
        bool changed = strcmp(s_manifest.tag, manifest->tag) ||
                       strcmp(s_manifest.s3.version, manifest->s3.version) ||
                       strcmp(s_manifest.bluetooth.version, manifest->bluetooth.version);
        s_manifest = *manifest;
        snprintf(s_status.tag, sizeof(s_status.tag), "%s", manifest->tag);
        refresh_available(&bluetooth);
        bool available = s_status.s3_available || s_status.bluetooth_available;
        if (manual || changed) {
            s_status.notice = available ? RELEASE_AVAILABLE : manual ? RELEASE_CURRENT : RELEASE_QUIET;
        }
    } else if (manual) {
        s_status.notice = RELEASE_ERROR;
    }
    taskEXIT_CRITICAL(&s_lock);
    free(manifest);
}

static void release_task(void *arg) {
    (void)arg;
    int64_t next_check = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000));
        github_release_status_t status;
        github_release_get_status(&status);
        bool manual = status.checking;
        if (!manual && esp_timer_get_time() < next_check) {
            continue;
        }
        char ip[16], ssid[33];
        if (!wifi_get_station(ssid, sizeof(ssid), ip, sizeof(ip)) || !ntp_is_synced()) {
            if (manual) {
                taskENTER_CRITICAL(&s_lock);
                s_status.checking = false;
                s_status.notice = RELEASE_ERROR;
                snprintf(s_status.error, sizeof(s_status.error), "Waiting for WiFi and clock sync");
                taskEXIT_CRITICAL(&s_lock);
            }
            continue;
        }
        ota_status_t ota;
        ota_get_status(&ota);
        if (ota.state == OTA_RECEIVING || ota.state == OTA_READY || ota.state == OTA_INSTALLING) {
            // Leave a manual request queued until the active update is done.
            continue;
        }
        taskENTER_CRITICAL(&s_lock);
        s_status.checking = true;
        taskEXIT_CRITICAL(&s_lock);
        check_release();
        github_release_get_status(&status);
        int64_t wait_us = status.error[0] ? RETRY_US : DAY_US;
        next_check = esp_timer_get_time() + wait_us;
        // Checks only run once SNTP has synced, so wall-clock time is valid here.
        int64_t now = time(NULL);
        taskENTER_CRITICAL(&s_lock);
        s_status.last_check = now;
        s_status.next_check = now + wait_us / 1000000;
        taskEXIT_CRITICAL(&s_lock);
    }
}

void github_release_start(void) {
    if (!s_task && xTaskCreate(release_task, "release_check", 8192, NULL, 2, &s_task) != pdPASS) {
        taskENTER_CRITICAL(&s_lock);
        s_status.notice = RELEASE_ERROR;
        snprintf(s_status.error, sizeof(s_status.error), "Couldn't start update checks: out of memory");
        taskEXIT_CRITICAL(&s_lock);
    }
}

bool github_release_check(void) {
    if (!s_task) {
        return false;
    }
    taskENTER_CRITICAL(&s_lock);
    bool ok = !s_status.checking;
    if (ok) {
        s_status.checking = true;
        s_status.notice = RELEASE_CHECKING;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (ok) {
        xTaskNotifyGive(s_task);
    }
    return ok;
}

bool github_release_download(ota_target_t target) {
    release_asset_t asset;
    hello_t bluetooth;
    uart_link_get_esp32_firmware(&bluetooth);
    taskENTER_CRITICAL(&s_lock);
    refresh_available(&bluetooth);
    bool available = !s_status.checking &&
                     (target == OTA_TARGET_S3 ? s_status.s3_available : s_status.bluetooth_available);
    asset = target == OTA_TARGET_S3 ? s_manifest.s3 : s_manifest.bluetooth;
    taskEXIT_CRITICAL(&s_lock);
    if (!available || !ota_download(asset.url, asset.size, target, asset.sha256)) {
        return false;
    }
    taskENTER_CRITICAL(&s_lock);
    s_status.notice = RELEASE_QUIET;
    taskEXIT_CRITICAL(&s_lock);
    return true;
}

void github_release_answer(bool download) {
    github_release_status_t status;
    github_release_get_status(&status);
    if (download && status.notice == RELEASE_AVAILABLE) {
        if (!github_release_download(status.target)) {
            taskENTER_CRITICAL(&s_lock);
            s_status.notice = RELEASE_ERROR;
            snprintf(s_status.error, sizeof(s_status.error), "Couldn't start download; try again");
            taskEXIT_CRITICAL(&s_lock);
        }
    } else {
        taskENTER_CRITICAL(&s_lock);
        s_status.notice = RELEASE_QUIET;
        taskEXIT_CRITICAL(&s_lock);
    }
}
