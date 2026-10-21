#include "http_get.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#define HTTP_TIMEOUT_MS 10000
#define RESPONSE_TIMEOUT_US (30LL * 1000000)

static const char *TAG = "http_get";

int http_get(const char *url, char *buf, size_t cap) {
    const esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        // Some free APIs refuse requests without one.
        .user_agent = "AudioKnob/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return -1;
    }

    int len = -1;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        int64_t deadline = esp_timer_get_time() + RESPONSE_TIMEOUT_US;
        int total = 0;
        int n;
        while (esp_timer_get_time() < deadline && total < (int)cap - 1 && (n = esp_http_client_read(client, buf + total, cap - 1 - total)) > 0) {
            total += n;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "%s: HTTP %d", url, status);
        } else if (!esp_http_client_is_complete_data_received(client)) {
            ESP_LOGW(TAG, "%s: body larger than %u bytes", url, (unsigned)cap - 1);
        } else {
            buf[total] = '\0';
            len = total;
        }
    } else {
        ESP_LOGW(TAG, "%s: connect failed", url);
    }
    esp_http_client_cleanup(client);
    return len;
}
