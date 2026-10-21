#include "mock_sdk.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

// Exercise the production release jobs through the real shared dispatcher
// with a simulated monotonic clock; no test-only hooks in the firmware.
#include "../../firmware/s3/src/device/net/network_jobs.c"
#include "../../firmware/s3/src/device/net/github_release.c"

static int64_t now;
static jmp_buf blocked;
static unsigned requests, fail_request;
static void (*queued_task)(void *);
static bool clock_ready;
static ota_state_t ota_state;
static bool slow_body;
static int allocations_fail;
static int64_t request_times[8];
static int fail_status;
static unsigned downloads;
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
    if (allocations_fail) { allocations_fail--; return NULL; }
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
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage) { *storage=1; return storage; }
int xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticks) {
    if (!*sem && !ticks) return 0;
    assert(*sem); *sem=0; return 1;
}
int xSemaphoreGive(SemaphoreHandle_t sem) { assert(!*sem); *sem=1; return 1; }
int esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) {
    assert(args->callback==dispatch); *out=(void *)1; return ESP_OK;
}
int esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t interval) {
    assert(timer && interval==TIMER_US); return ESP_OK;
}
void vTaskDelay(uint32_t ticks) { assert(ticks==1); }
TaskHandle_t xTaskCreateStatic(void (*fn)(void *), const char *name, unsigned stack, void *arg,
                             unsigned priority, StackType_t *buffer, StaticTask_t *tcb) {
    assert(fn==worker && !strcmp(name,"network_jobs") && stack==8192 && !arg && priority==2);
    assert(buffer==s_stack && tcb==&s_task_storage);
    assert(!queued_task); queued_task=fn; return (void *)1;
}
uint32_t ulTaskNotifyTake(int clear, uint32_t ticks) {
    assert(clear==pdTRUE && ticks==portMAX_DELAY && s_waiting); longjmp(blocked,1);
}
void xTaskNotifyGive(TaskHandle_t task) { assert(task==s_worker); queued_task=worker; }
static void pump(void) {
    dispatch(NULL);
    if (queued_task) { void (*fn)(void *)=queued_task; queued_task=NULL;
        if (!setjmp(blocked)) fn(NULL); }
    assert(!s_worker || s_waiting);
}
bool wifi_get_station(char *ssid, size_t ssid_len, char *ip, size_t ip_len) {
    (void)ssid;
    (void)ssid_len;
    (void)ip;
    (void)ip_len;
    return now >= 5LL * 60 * 1000000;
}
bool ntp_is_synced(void) { return clock_ready; }
void ota_get_status(ota_status_t *out) { memset(out, 0, sizeof(*out)); out->state=ota_state; }
bool ota_download_reserve(ota_target_t target) {
    (void)target;
    if (ota_state==OTA_RECEIVING || ota_state==OTA_READY || ota_state==OTA_INSTALLING) return false;
    ota_state=OTA_RECEIVING; return true;
}
void ota_download(const char *url, size_t size, ota_target_t target, const uint8_t sha256[32]) {
    for (unsigned i = 0; i < 32; i++) {
        assert(sha256[i] == 1);
    }
    assert(strstr(url, target == OTA_TARGET_S3 ? "firmware-s3.bin" : "firmware-esp32.bin"));
    assert(size == (target == OTA_TARGET_S3 ? 4096 : 1024));
    assert(ota_state==OTA_RECEIVING); downloads++; ota_state=OTA_READY;
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
    if (slow_body) { now+=16LL*1000000; len=1; }
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
    network_jobs_init();
    github_release_start();
    github_release_start(); // Idempotent startup registration.
    pump(); assert(!requests && !s_worker);
    network_jobs_set_network(true);
    pump(); assert(!requests); // Clock readiness is also required.
    clock_ready=true; ota_state=OTA_READY;
    pump(); assert(!requests); // Startup is deferred during active OTA.
    ota_state=OTA_IDLE; now=60LL*1000000;
    pump(); assert(requests==1 && request_times[0]==now);
    now+=3LL*24*60*60*1000000; pump();
    assert(requests==1); // No permanent task, daily polling or hourly retry.
    github_release_status_t status;
    github_release_get_status(&status);
    assert(status.notice == RELEASE_AVAILABLE && status.s3_available && status.bluetooth_available);
    assert(!status.checking && status.checked && !status.error[0]);
    assert(status.last_check>0 && status.next_check==0);
    github_release_answer(false);
    pump();
    github_release_get_status(&status);
    assert(status.notice == RELEASE_QUIET); // Dismissal survives idle worker blocking.
    assert(github_release_check());
    assert(!github_release_check());

    pump();
    github_release_get_status(&status);
    assert(status.notice == RELEASE_AVAILABLE); // Manual check shows the same update again.
    github_release_answer(true);
    assert(downloads==0 && ota_state==OTA_RECEIVING);
    assert(!github_release_download(OTA_TARGET_S3));
    pump(); assert(downloads == 1);
    ota_state=OTA_DECLINED;
    github_release_get_status(&status);
    assert(status.notice == RELEASE_QUIET);
    running = "1.2.3";
    github_release_get_status(&status);
    assert(!status.s3_available && status.bluetooth_available && status.target == OTA_TARGET_ESP32);
    assert(github_release_download(OTA_TARGET_ESP32));
    pump(); assert(downloads == 2);
    ota_state=OTA_DECLINED;
    bt_running = "v1.2.3";
    assert(github_release_check());
    pump();
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
    pump();
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
    pump();
    github_release_get_status(&status);
    assert(status.notice == RELEASE_ERROR && strstr(status.error, "rate limit"));
    // A failed startup/manual HTTP result is retained without periodic retries.
    unsigned previous=requests;
    now+=3LL*24*60*60*1000000; pump(); assert(requests==previous);
    // Offline manual requests coalesce and wait for readiness instead of being lost.
    network_jobs_set_network(false);
    assert(github_release_check()); assert(!github_release_check());
    pump(); assert(requests==previous);
    network_jobs_set_network(true); ota_state=OTA_RECEIVING;
    pump(); assert(requests==previous);
    ota_state=OTA_IDLE; now+=60LL*1000000; pump(); assert(requests==previous+1);
    fail_status=0; slow_body=true;
    assert(github_release_check()); pump();
    github_release_get_status(&status);
    assert(status.notice==RELEASE_ERROR && strstr(status.error,"Incomplete"));
    slow_body=false; allocations_fail=1;
    assert(github_release_check()); pump();
    github_release_get_status(&status);
    assert(status.notice==RELEASE_ERROR && strstr(status.error,"memory"));
    assert(github_release_check()); pump();
    github_release_get_status(&status);
    assert(!status.checking && !status.error[0]);
    puts("Startup/manual discovery, readiness, OTA deferral, retained results, deadlines and dismissal passed");
}
