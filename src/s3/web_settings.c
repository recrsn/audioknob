#include "web_settings.h"

#include <stdio.h>
#include <string.h>

#include "device_name.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "web_page.h"

#define FORM_MAX 256
#define HEADER_MAX 96

static const char *TAG = "web_settings";

static httpd_handle_t s_server;

static esp_err_t root_handler(httpd_req_t *req) {
    char query[32] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));

    char name[DEVICE_NAME_MAX + 1], hostname[64];
    device_name_get(name, sizeof(name));
    device_name_hostname(hostname, sizeof(hostname));

    web_page_t page;
    if (!web_page_begin(&page, name, "")) {
        return httpd_resp_send_500(req);
    }
    web_page_append(&page, "<h1>");
    web_page_append_escaped(&page, name);
    web_page_append(&page, "</h1>");
    if (strstr(query, "saved")) {
        web_page_append(&page, "<p class=\"ok\">Saved. This page is now at "
                               "<a href=\"http://%s.local/\">%s.local</a>.</p>",
                        hostname, hostname);
    } else if (strstr(query, "invalid")) {
        web_page_append(&page, "<p class=\"err\">Names need 1 to %d characters.</p>", DEVICE_NAME_MAX);
    }
    web_page_append(&page, "<form method=\"post\" action=\"/name\">"
                           "<label for=\"name\">Device name</label>"
                           "<input id=\"name\" name=\"name\" maxlength=\"%d\" required value=\"",
                    DEVICE_NAME_MAX);
    web_page_append_escaped(&page, name);
    web_page_append(&page, "\"><p class=\"hint\">Shown in AirPlay and Bluetooth lists. Phones already paired over "
                           "Bluetooth may keep the old name until you pair again.</p>"
                           "<button type=\"submit\">Save</button></form>");
    return web_page_send(req, &page);
}

// Any web page the user visits could otherwise post here from their
// browser. Browsers send Origin on cross-site POSTs; same-origin ones
// match Host.
static bool same_origin(httpd_req_t *req) {
    char origin[HEADER_MAX], host[HEADER_MAX], expected[HEADER_MAX + 8];
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) != ESP_OK) {
        return true;
    }
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        return false;
    }
    snprintf(expected, sizeof(expected), "http://%s", host);
    return !strcmp(origin, expected);
}

static esp_err_t name_handler(httpd_req_t *req) {
    if (!same_origin(req)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-site request refused");
    }
    char form[FORM_MAX + 1];
    if (!web_read_form(req, form, sizeof(form))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    }
    char name[DEVICE_NAME_MAX * 3 + 1];
    web_form_value(form, "name", name, sizeof(name));
    return web_redirect(req, device_name_set(name) ? "/?saved=1" : "/?invalid=1");
}

void web_settings_start(void) {
    if (s_server) {
        return;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.stack_size = 4096;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        s_server = NULL;
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/name", .method = HTTP_POST, .handler = name_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }
    ESP_LOGI(TAG, "device page up");
}
