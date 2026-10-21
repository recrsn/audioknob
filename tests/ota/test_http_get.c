#include "mock_sdk.h"
#include "online/http_get.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int64_t now;
static int reads, cleanups, status=200;
static bool fail_init, fail_open, trickle;
static size_t offset;
static const char *body="weather-json";
int64_t esp_timer_get_time(void) { return now; }
int esp_crt_bundle_attach(void *arg) { (void)arg; return 0; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    assert(config->timeout_ms==10000 && config->crt_bundle_attach==esp_crt_bundle_attach);
    offset=0; return fail_init ? NULL : (void *)1;
}
int esp_http_client_open(esp_http_client_handle_t client, int len) {
    assert(client && !len); return fail_open ? -1 : ESP_OK;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client) { assert(client); return (int64_t)strlen(body); }
int esp_http_client_get_status_code(esp_http_client_handle_t client) { assert(client); return status; }
int esp_http_client_read(esp_http_client_handle_t client, char *buf, size_t size) {
    assert(client); reads++; assert(reads<100);
    if (trickle) { now+=9000000; size=1; }
    size_t left=strlen(body)-offset;
    if (size>left) size=left;
    memcpy(buf,body+offset,size); offset+=size; return (int)size;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client) {
    assert(client); return offset==strlen(body);
}
int esp_http_client_cleanup(esp_http_client_handle_t client) { assert(client); cleanups++; return ESP_OK; }
int main(void) {
    char buf[32];
    assert(http_get("https://weather.test",buf,sizeof(buf))==(int)strlen(body));
    assert(!strcmp(buf,body) && cleanups==1);
    status=500; assert(http_get("https://weather.test",buf,sizeof(buf))<0 && cleanups==2);
    status=200; fail_open=true;
    assert(http_get("https://weather.test",buf,sizeof(buf))<0 && cleanups==3);
    fail_open=false; fail_init=true;
    assert(http_get("https://weather.test",buf,sizeof(buf))<0 && cleanups==3);
    fail_init=false; body="longer-than-output";
    assert(http_get("https://weather.test",buf,4)<0 && cleanups==4);
    body="response-that-never-finishes-on-time"; trickle=true; reads=0;
    assert(http_get("https://weather.test",buf,sizeof(buf))<0 && cleanups==5);
    assert(reads==4 && now==36000000); // Deadline plus at most one 10 s read.
    puts("Weather/geolocation HTTP success, errors, byte cap, slow-body deadline and cleanup passed");
}
