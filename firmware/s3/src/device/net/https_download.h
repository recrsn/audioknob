#pragma once

#include "esp_http_client.h"

// Verified HTTPS GET, following at most five redirects. Returns an open
// HTTP 200 response; caller owns cleanup. Never follows a downgrade to HTTP.
esp_http_client_handle_t https_download_open(const char *url, int *status);
