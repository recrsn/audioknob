#include "wifi_portal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define DNS_PORT 53
#define DNS_MAX_PACKET 512
#define DNS_HEADER_LEN 12
#define DNS_TTL_S 60

#define MAX_NETWORKS 20
#define PAGE_SIZE 8192
#define FORM_MAX 512

static const char *TAG = "wifi_portal";

static const wifi_portal_ops_t *s_ops;
static uint32_t s_ap_ip;
static char s_ap_url[32];

// --- DNS: answer every A query with the AP's address -----------------------

static void dns_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        vTaskDelete(NULL);
        return;
    }

    uint8_t pkt[DNS_MAX_PACKET + 16];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, pkt, DNS_MAX_PACKET, 0, (struct sockaddr *)&from, &from_len);
        if (len <= DNS_HEADER_LEN || (pkt[2] & 0x80)) {
            continue;  // too short, or a response rather than a query
        }

        // Walk the first question's QNAME to find QTYPE/QCLASS.
        int pos = DNS_HEADER_LEN;
        while (pos < len && pkt[pos] != 0) {
            pos += pkt[pos] + 1;
        }
        if (pos + 5 > len) {
            continue;
        }
        uint16_t qtype = (pkt[pos + 1] << 8) | pkt[pos + 2];
        int answer_at = pos + 5;

        // Response: same ID and question, QR + AA set, RD echoed, one
        // question, and an answer only for A queries (AAAA etc. get none).
        pkt[2] = 0x84 | (pkt[2] & 0x01);
        pkt[3] = 0x00;
        pkt[4] = 0x00;
        pkt[5] = 0x01;
        pkt[6] = 0x00;
        pkt[7] = qtype == 1 ? 1 : 0;
        memset(&pkt[8], 0, 4);  // drop authority/additional (e.g. EDNS OPT)

        int out_len = answer_at;
        if (qtype == 1) {
            const uint8_t answer[] = {
                0xC0, 0x0C,              // pointer to the QNAME
                0x00, 0x01, 0x00, 0x01,  // type A, class IN
                0x00, 0x00, 0x00, DNS_TTL_S,
                0x00, 0x04,
            };
            memcpy(&pkt[out_len], answer, sizeof(answer));
            out_len += sizeof(answer);
            memcpy(&pkt[out_len], &s_ap_ip, 4);
            out_len += 4;
        }
        sendto(sock, pkt, out_len, 0, (struct sockaddr *)&from, from_len);
    }
}

// --- HTML helpers ----------------------------------------------------------

typedef struct {
    char *buf;
    size_t len;
} page_t;

static void page_append(page_t *page, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(page->buf + page->len, PAGE_SIZE - page->len, fmt, args);
    va_end(args);
    if (n > 0) {
        page->len += (size_t)n < PAGE_SIZE - page->len ? (size_t)n : PAGE_SIZE - page->len - 1;
    }
}

// SSIDs are attacker-controlled (anyone can broadcast one), so escape them.
static void page_append_escaped(page_t *page, const char *text) {
    for (; *text && page->len < PAGE_SIZE - 8; text++) {
        switch (*text) {
        case '<': page_append(page, "&lt;"); break;
        case '>': page_append(page, "&gt;"); break;
        case '&': page_append(page, "&amp;"); break;
        case '"': page_append(page, "&quot;"); break;
        case '\'': page_append(page, "&#39;"); break;
        default: page->buf[page->len++] = *text; page->buf[page->len] = '\0'; break;
        }
    }
}

static void page_begin(page_t *page, const char *head_extra) {
    page_append(page,
                "<!doctype html><html><head><meta charset=\"utf-8\">"
                "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                "<title>AudioKnob setup</title>%s<style>"
                "body{font-family:-apple-system,system-ui,sans-serif;background:#0b0b10;color:#eee;"
                "max-width:420px;margin:0 auto;padding:24px 16px}"
                "h1{font-size:22px;margin:0 0 16px}label{display:block;margin:14px 0 6px;color:#aaa}"
                "select,input{width:100%%;box-sizing:border-box;padding:12px;font-size:16px;border-radius:10px;"
                "border:1px solid #333;background:#16161e;color:#eee}"
                "button{width:100%%;margin-top:20px;padding:14px;font-size:17px;border:0;border-radius:10px;"
                "background:#4ea1ff;color:#fff}a{color:#4ea1ff}p{line-height:1.5}"
                "</style></head><body>",
                head_extra);
}

static esp_err_t page_send(httpd_req_t *req, page_t *page) {
    page_append(page, "</body></html>");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page->buf, page->len);
}

// In-place application/x-www-form-urlencoded decode.
static void url_decode(char *s) {
    char *out = s;
    for (; *s; s++) {
        if (*s == '+') {
            *out++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char hex[3] = {s[1], s[2], '\0'};
            *out++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *out++ = *s;
        }
    }
    *out = '\0';
}

// --- Handlers --------------------------------------------------------------

static esp_err_t root_handler(httpd_req_t *req) {
    char query[32] = "";
    bool refresh = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK && strstr(query, "rescan");

    wifi_network_t *networks = calloc(MAX_NETWORKS, sizeof(wifi_network_t));
    page_t page = {.buf = malloc(PAGE_SIZE)};
    if (!networks || !page.buf) {
        free(networks);
        free(page.buf);
        return httpd_resp_send_500(req);
    }
    page.buf[0] = '\0';
    size_t count = s_ops->scan(networks, MAX_NETWORKS, refresh);

    page_begin(&page, "");
    page_append(&page, "<h1>Connect AudioKnob to WiFi</h1><form method=\"post\" action=\"/connect\">"
                       "<label for=\"ssid\">Network</label><select id=\"ssid\" name=\"ssid\">");
    for (size_t i = 0; i < count; i++) {
        page_append(&page, "<option value=\"");
        page_append_escaped(&page, networks[i].ssid);
        page_append(&page, "\">");
        page_append_escaped(&page, networks[i].ssid);
        page_append(&page, "%s</option>", networks[i].secure ? "" : " (open)");
    }
    page_append(&page, "</select>"
                       "<label for=\"other\">Or a hidden network</label>"
                       "<input id=\"other\" name=\"other\" autocapitalize=\"none\" autocorrect=\"off\">"
                       "<label for=\"password\">Password</label>"
                       "<input id=\"password\" name=\"password\" type=\"password\">"
                       "<button type=\"submit\">Connect</button></form>"
                       "<p><a href=\"/?rescan=1\">Scan again</a></p>");

    esp_err_t err = page_send(req, &page);
    free(networks);
    free(page.buf);
    return err;
}

static esp_err_t connect_handler(httpd_req_t *req) {
    char form[FORM_MAX + 1];
    int total = 0;
    while (total < req->content_len && total < FORM_MAX) {
        int n = httpd_req_recv(req, form + total, FORM_MAX - total);
        if (n <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
        }
        total += n;
    }
    form[total] = '\0';

    char ssid[33 * 3] = "", other[33 * 3] = "", password[65 * 3] = "";
    httpd_query_key_value(form, "ssid", ssid, sizeof(ssid));
    httpd_query_key_value(form, "other", other, sizeof(other));
    httpd_query_key_value(form, "password", password, sizeof(password));
    url_decode(ssid);
    url_decode(other);
    url_decode(password);

    const char *chosen = other[0] ? other : ssid;
    if (!chosen[0] || strlen(chosen) > 32 || strlen(password) > 64) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid network name or password");
    }
    s_ops->submit(chosen, password);

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/status");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req) {
    page_t page = {.buf = malloc(PAGE_SIZE)};
    if (!page.buf) {
        return httpd_resp_send_500(req);
    }
    page.buf[0] = '\0';

    char status[96];
    s_ops->status(status, sizeof(status));

    page_begin(&page, "<meta http-equiv=\"refresh\" content=\"2\">");
    page_append(&page, "<h1>AudioKnob</h1><p>");
    page_append_escaped(&page, status);
    page_append(&page, "</p><p>The device screen shows the same progress. Once it connects it restarts "
                       "and this setup network goes away.</p><p><a href=\"/\">Back</a></p>");

    esp_err_t err = page_send(req, &page);
    free(page.buf);
    return err;
}

// Everything else (including iOS/Android captive-portal probes) goes to the
// setup page, which is what makes the phone pop it up on its own.
static esp_err_t redirect_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", s_ap_url);
    return httpd_resp_send(req, NULL, 0);
}

static void http_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return;
    }

    // First match wins, so the catch-all goes last.
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/connect", .method = HTTP_POST, .handler = connect_handler},
        {.uri = "/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = redirect_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}

void wifi_portal_start(uint32_t ap_ip, const wifi_portal_ops_t *ops) {
    s_ops = ops;
    s_ap_ip = ap_ip;
    struct in_addr addr = {.s_addr = ap_ip};
    snprintf(s_ap_url, sizeof(s_ap_url), "http://%s/", inet_ntoa(addr));

    xTaskCreate(dns_task, "dns_portal", 3072, NULL, 4, NULL);
    http_start();
    ESP_LOGI(TAG, "captive portal up at %s", s_ap_url);
}
