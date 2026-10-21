#include "update/github_release.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "net/network_jobs.h"
#include "update/https_download.h"
#include "update/release_manifest.h"
#include "sdkconfig.h"
#include "drivers/uart_link.h"

#define RELEASE_JSON_CAP (64 * 1024)
#define RELEASE_BODY_TIMEOUT_US (30LL * 1000000)

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_started;
static github_release_status_t s_status;
static release_manifest_t s_manifest;
// Downloads run on the shared network task: its internal stack is already there,
// where a new one may not fit once AirPlay has fragmented internal RAM.
static release_asset_t s_download;
static ota_target_t s_download_target;
static bool s_download_pending;

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
        int64_t deadline = esp_timer_get_time() + RELEASE_BODY_TIMEOUT_US;
        size_t total = 0;
        int n = 0;
        while (esp_timer_get_time() < deadline && total < RELEASE_JSON_CAP - 1 &&
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
    int64_t checked_at = time(NULL);
    taskENTER_CRITICAL(&s_lock);
    bool manual = s_status.notice == RELEASE_CHECKING;
    s_status.checking = false;
    s_status.checked = true;
    s_status.last_check = checked_at;
    s_status.next_check = 0; // Startup/manual discovery has no periodic deadline.
    snprintf(s_status.error, sizeof(s_status.error), "%s", error ? error : "");
    if (!error) {
        // Dismissal lasts for this release and boot; a manual check can show it again.
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

static void release_job(void) {
    ota_status_t ota;
    ota_get_status(&ota);
    if (ota.state == OTA_RECEIVING || ota.state == OTA_READY || ota.state == OTA_INSTALLING) {
        // Retain startup/manual intent, without holding a stack while OTA owns
        // the update path. This is deferral, not periodic release discovery.
        network_jobs_schedule(NETWORK_JOB_RELEASE, 60);
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    s_status.checking = true;
    taskEXIT_CRITICAL(&s_lock);
    check_release();
    // One startup attempt; further discovery is explicitly requested in Settings.
}

static void download_job(void) {
    taskENTER_CRITICAL(&s_lock);
    bool pending = s_download_pending;
    s_download_pending = false;
    taskEXIT_CRITICAL(&s_lock);
    if (pending) {
        // The OTA reservation keeps this asset stable and excludes other
        // downloads. Flash writes run on the shared internal stack.
        ota_download(s_download.url, s_download.size, s_download_target, s_download.sha256);
    }
}

void github_release_start(void) {
    taskENTER_CRITICAL(&s_lock);
    bool start = !s_started;
    s_started = true;
    taskEXIT_CRITICAL(&s_lock);
    if (start) {
        network_jobs_register(NETWORK_JOB_RELEASE, release_job, true);
        network_jobs_register(NETWORK_JOB_RELEASE_DOWNLOAD, download_job, false);
        network_jobs_schedule(NETWORK_JOB_RELEASE, 0);
    }
}

bool github_release_check(void) {
    taskENTER_CRITICAL(&s_lock);
    bool ok = s_started && !s_status.checking;
    if (ok) {
        s_status.checking = true;
        s_status.notice = RELEASE_CHECKING;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (ok) {
        network_jobs_schedule(NETWORK_JOB_RELEASE, 0);
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
    if (!available || !s_started || !ota_download_reserve(target)) {
        return false;
    }
    taskENTER_CRITICAL(&s_lock);
    // The reservation keeps any other download out until this one finishes.
    s_download = asset;
    s_download_target = target;
    s_download_pending = true;
    s_status.notice = RELEASE_QUIET;
    taskEXIT_CRITICAL(&s_lock);
    network_jobs_schedule(NETWORK_JOB_RELEASE_DOWNLOAD, 0);
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
