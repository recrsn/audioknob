#include "https_download.h"

#include "esp_crt_bundle.h"
#include <strings.h>

esp_http_client_handle_t https_download_open(const char *url, int *status) {
    *status = 0;
    if (strncasecmp(url, "https://", 8)) {
        return NULL;
    }
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "AudioKnob/1.0",
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return NULL;
    }
    esp_http_client_set_header(client, "Accept", "*/*");
    for (unsigned redirects = 0; redirects <= 5; redirects++) {
        if (esp_http_client_get_transport_type(client) != HTTP_TRANSPORT_OVER_SSL ||
            esp_http_client_open(client, 0) != ESP_OK || esp_http_client_fetch_headers(client) < 0) {
            break;
        }
        *status = esp_http_client_get_status_code(client);
        if (*status == 200) {
            return client;
        }
        if ((*status != 301 && *status != 302 && *status != 303 && *status != 307 && *status != 308) ||
            redirects == 5 || esp_http_client_set_redirection(client) != ESP_OK) {
            break;
        }
        esp_http_client_close(client);
    }
    esp_http_client_cleanup(client);
    return NULL;
}
