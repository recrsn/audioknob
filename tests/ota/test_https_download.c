#include "mock_sdk.h"
#include "https_download.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static esp_http_client_config_t config;
static char signed_url[1100];
static const char *current_url;
static unsigned opens, redirects, closes, cleanups;
static bool loop_redirect, downgrade, fail_open, fail_headers;
static int final_status;

int esp_crt_bundle_attach(void *conf) {
    (void)conf;
    return ESP_OK;
}
const char *esp_err_to_name(esp_err_t error) {
    (void)error;
    return "mock error";
}
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
    config = *cfg;
    current_url = cfg->url;
    assert(cfg->crt_bundle_attach == esp_crt_bundle_attach);
    assert(cfg->disable_auto_redirect);
    return (void *)1;
}
int esp_http_client_set_header(esp_http_client_handle_t client, const char *name, const char *value) {
    (void)client;
    (void)name;
    (void)value;
    return ESP_OK;
}
int esp_http_client_get_transport_type(esp_http_client_handle_t client) {
    (void)client;
    return !strncmp(current_url, "https://", 8) ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP;
}
int esp_http_client_open(esp_http_client_handle_t client, int write_len) {
    (void)client;
    assert(write_len == 0);
    opens++;
    // Model the ESP-IDF request-line limit using the URL path and query.
    const char *path = strchr(current_url + strlen("https://"), '/');
    size_t line_size = strlen("GET ") + strlen(path) + strlen(" HTTP/1.1\r\n");
    size_t tx_size = config.buffer_size_tx ? (size_t)config.buffer_size_tx : 512;
    return fail_open || line_size >= tx_size ? -1 : ESP_OK;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client) {
    (void)client;
    return fail_headers ? -1 : 1234;
}
int esp_http_client_get_status_code(esp_http_client_handle_t client) {
    (void)client;
    return opens == 1 || loop_redirect ? 302 : final_status;
}
int esp_http_client_set_redirection(esp_http_client_handle_t client) {
    (void)client;
    redirects++;
    current_url = downgrade ? "http://example.invalid/firmware.bin" : signed_url;
    return ESP_OK;
}
int esp_http_client_close(esp_http_client_handle_t client) {
    (void)client;
    closes++;
    return ESP_OK;
}
int esp_http_client_cleanup(esp_http_client_handle_t client) {
    (void)client;
    cleanups++;
    return ESP_OK;
}

static void reset(void) {
    opens = redirects = closes = cleanups = 0;
    loop_redirect = downgrade = fail_open = fail_headers = false;
    final_status = 200;
}

int main(void) {
    const char *url = "https://github.com/recrsn/audioknob/releases/download/v0.1.1/firmware-s3.bin";
    const char *prefix = "https://release-assets.githubusercontent.com/asset.bin?token=";
    strcpy(signed_url, prefix);
    memset(signed_url + strlen(prefix), 'x', 900);
    signed_url[strlen(prefix) + 900] = '\0';
    int status;
    reset();
    esp_http_client_handle_t client = https_download_open(url, &status);
    assert(client && status == 200 && opens == 2 && redirects == 1);
    assert(config.buffer_size_tx > 512 && cleanups == 0);
    // The exact same redirect fails with ESP-IDF's default buffer.
    config.buffer_size_tx = 512;
    assert(esp_http_client_open(client, 0) != ESP_OK);
    esp_http_client_cleanup(client);

    reset();
    downgrade = true;
    assert(!https_download_open(url, &status));
    assert(opens == 1 && cleanups == 1); // Never connects to downgraded HTTP.
    reset();
    loop_redirect = true;
    assert(!https_download_open(url, &status));
    assert(opens == 6 && redirects == 5 && cleanups == 1);
    reset();
    fail_open = true;
    assert(!https_download_open(url, &status) && cleanups == 1);
    reset();
    fail_headers = true;
    assert(!https_download_open(url, &status) && cleanups == 1);
    reset();
    final_status = 404;
    assert(!https_download_open(url, &status) && status == 404 && cleanups == 1);
    reset();
    assert(!https_download_open("http://example.invalid/fw.bin", &status) && opens == 0);
    puts("Long GitHub CDN redirect, HTTPS enforcement, redirect limits and cleanup passed");
}
