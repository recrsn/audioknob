#include "mock_sdk.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

// Include the implementation to exercise the real scheduler loop with a
// simulated monotonic clock; no test-only hooks in the firmware.
#include "../../firmware/s3/src/device/net/github_release.c"

static jmp_buf stop;
static int64_t now;
static unsigned iterations, stop_after, requests, fail_request;
static int64_t request_times[8];
static int fail_status;
static unsigned downloads, notifications;
static size_t read_offset;
static const char *running = "1.0.0", *bt_running = "1.0.0";
static const char *body = "{\"tag_name\":\"v1.2.3\",\"assets\":["
                          "{\"name\":\"firmware-s3.bin\",\"digest\":\"sha256:"
                          "0101010101010101010101010101010101010101010101010101010101010101\",\"size\":4096,"
                          "\"browser_download_url\":\"https://"
                          "github.com/recrsn/audioknob/releases/download/v1.2.3/firmware-s3.bin\"},"
                          "{\"name\":\"firmware-esp32.bin\",\"digest\":\"sha256:"
                          "0101010101010101010101010101010101010101010101010101010101010101\",\"size\":1024,"
                          "\"browser_download_url\":\"https://"
                          "github.com/recrsn/audioknob/releases/download/v1.2.3/firmware-esp32.bin\"}]}";

void *heap_caps_malloc(size_t size, unsigned caps) {
    (void)caps;
    return malloc(size);
}
const esp_app_desc_t *esp_app_get_description(void) {
    static esp_app_desc_t desc;
    snprintf(desc.version, sizeof(desc.version), "%s", running);
    return &desc;
}
uint32_t uart_link_get_esp32_firmware(hello_t *out) {
    snprintf(out->version, sizeof(out->version), "%s", bt_running);
    return 1;
}
int64_t esp_timer_get_time(void) { return now; }
uint32_t ulTaskNotifyTake(int clear, uint32_t ticks) {
    assert(clear == pdTRUE && ticks == 60000);
    if (++iterations > stop_after) {
        longjmp(stop, 1);
    }
    now += 60LL * 1000000;
    return 0;
}
void xTaskNotifyGive(TaskHandle_t task) {
    assert(task);
    notifications++;
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *arg, unsigned priority,
                void *out) {
    (void)fn;
    (void)name;
    (void)stack;
    (void)arg;
    (void)priority;
    *(TaskHandle_t *)out = (void *)1;
    return pdPASS;
}
bool wifi_get_station(char *ssid, size_t ssid_len, char *ip, size_t ip_len) {
    (void)ssid;
    (void)ssid_len;
    (void)ip;
    (void)ip_len;
    return now >= 5LL * 60 * 1000000;
}
bool ntp_is_synced(void) { return now >= 10LL * 60 * 1000000; }
void ota_get_status(ota_status_t *out) { memset(out, 0, sizeof(*out)); }
bool ota_download(const char *url, size_t size, ota_target_t target, const uint8_t sha256[32]) {
    for (unsigned i = 0; i < 32; i++) {
        assert(sha256[i] == 1);
    }
    assert(strstr(url, target == OTA_TARGET_S3 ? "firmware-s3.bin" : "firmware-esp32.bin"));
    assert(size == (target == OTA_TARGET_S3 ? 4096 : 1024));
    downloads++;
    return true;
}
esp_http_client_handle_t https_download_open(const char *url, int *status) {
    assert(!strcmp(url, "https://api.github.com/repos/recrsn/audioknob/releases/latest"));
    if (requests < 8) {
        request_times[requests] = now;
    }
    requests++;
    *status = fail_status ? fail_status : 200;
    read_offset = 0;
    return requests == fail_request || fail_status ? NULL : (void *)1;
}
int esp_http_client_read(esp_http_client_handle_t client, char *buf, size_t len) {
    (void)client;
    size_t left = strlen(body) - read_offset;
    if (len > left) {
        len = left;
    }
    memcpy(buf, body + read_offset, len);
    read_offset += len;
    return (int)len;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client) {
    (void)client;
    return read_offset == strlen(body);
}
int esp_http_client_cleanup(esp_http_client_handle_t client) {
    (void)client;
    return ESP_OK;
}

int main(void) {
    github_release_start();
    assert(s_task);
    fail_request = 2;
    stop_after = 1511;
    if (!setjmp(stop)) {
        release_task(NULL);
    }
    assert(requests == 3);
    assert(request_times[0] == 10LL * 60 * 1000000); // Wait for both WiFi and clock.
    assert(request_times[1] - request_times[0] == DAY_US);
    assert(request_times[2] - request_times[1] == RETRY_US);
    github_release_status_t status;
    github_release_get_status(&status);
    assert(status.notice == RELEASE_AVAILABLE && status.s3_available && status.bluetooth_available);
    assert(!status.checking && status.checked && !status.error[0]);
    github_release_answer(false);
    check_release();
    github_release_get_status(&status);
    assert(status.notice == RELEASE_QUIET); // Daily check respects dismissal.
    assert(github_release_check());
    assert(!github_release_check());
    assert(notifications == 1);
    check_release();
    github_release_get_status(&status);
    assert(status.notice == RELEASE_AVAILABLE); // Manual check shows the same update again.
    github_release_answer(true);
    assert(downloads == 1);
    github_release_get_status(&status);
    assert(status.notice == RELEASE_QUIET);
    running = "1.2.3";
    github_release_get_status(&status);
    assert(!status.s3_available && status.bluetooth_available && status.target == OTA_TARGET_ESP32);
    assert(github_release_download(OTA_TARGET_ESP32));
    assert(downloads == 2);
    bt_running = "v1.2.3";
    assert(github_release_check());
    check_release();
    github_release_get_status(&status);
    assert(status.notice == RELEASE_CURRENT && !status.s3_available && !status.bluetooth_available);
    assert(!github_release_download(OTA_TARGET_ESP32));
    body = "{\"tag_name\":\"v1.3.0\",\"assets\":["
           "{\"name\":\"firmware-s3.bin\",\"size\":4096,\"digest\":\"sha256:"
           "0101010101010101010101010101010101010101010101010101010101010101\","
           "\"browser_download_url\":\"https://github.com/recrsn/audioknob/releases/download/v1.3.0/"
           "firmware-s3.bin\"},"
           "{\"name\":\"firmware-esp32.bin\",\"label\":\"version:v1.2.3\",\"size\":1024,"
           "\"digest\":\"sha256:0101010101010101010101010101010101010101010101010101010101010101\","
           "\"browser_download_url\":\"https://github.com/recrsn/audioknob/releases/download/v1.3.0/"
           "firmware-esp32.bin\"}]}";
    assert(github_release_check());
    check_release();
    github_release_get_status(&status);
    assert(status.s3_available && !status.bluetooth_available);
    assert(!strcmp(status.s3_version, "v1.3.0") && !strcmp(status.bluetooth_version, "v1.2.3"));
    // A device that skipped the Bluetooth release can still update from the latest release.
    running = "v1.3.0";
    bt_running = "v1.0.0";
    github_release_get_status(&status);
    assert(!status.s3_available && status.bluetooth_available && status.target == OTA_TARGET_ESP32);
    fail_status = 429;
    assert(github_release_check());
    check_release();
    github_release_get_status(&status);
    assert(status.notice == RELEASE_ERROR && strstr(status.error, "rate limit"));
    puts("Daily scheduling, hourly retries, connectivity gates, manual checks and dismissal passed");
}
