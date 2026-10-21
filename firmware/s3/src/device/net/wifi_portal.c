#include "wifi_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_name.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "web_page.h"

#define DNS_PORT 53
#define DNS_MAX_PACKET 512
#define DNS_HEADER_LEN 12
#define DNS_TTL_S 60

#define MAX_NETWORKS 20
#define FORM_MAX 1024

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

// --- Handlers --------------------------------------------------------------

// Icons, defined once per page and drawn with <use>.
static const char ICONS[] =
    "<svg width=\"0\" height=\"0\" style=\"position:absolute\">"
    "<symbol id=\"w1\" viewBox=\"0 0 24 24\"><path d=\"M12 21l3.5-3.5a5 5 0 0 0-7 0z\"/>"
    "<path opacity=\".3\" d=\"M5 13.5l2 2a7 7 0 0 1 10 0l2-2a10 10 0 0 0-14 0z\"/>"
    "<path opacity=\".3\" d=\"M1 9.5l2 2a12.7 12.7 0 0 1 18 0l2-2a15.5 15.5 0 0 0-22 0z\"/></symbol>"
    "<symbol id=\"w2\" viewBox=\"0 0 24 24\"><path d=\"M12 21l3.5-3.5a5 5 0 0 0-7 0z\"/>"
    "<path d=\"M5 13.5l2 2a7 7 0 0 1 10 0l2-2a10 10 0 0 0-14 0z\"/>"
    "<path opacity=\".3\" d=\"M1 9.5l2 2a12.7 12.7 0 0 1 18 0l2-2a15.5 15.5 0 0 0-22 0z\"/></symbol>"
    "<symbol id=\"w3\" viewBox=\"0 0 24 24\"><path d=\"M12 21l3.5-3.5a5 5 0 0 0-7 0z\"/>"
    "<path d=\"M5 13.5l2 2a7 7 0 0 1 10 0l2-2a10 10 0 0 0-14 0z\"/>"
    "<path d=\"M1 9.5l2 2a12.7 12.7 0 0 1 18 0l2-2a15.5 15.5 0 0 0-22 0z\"/></symbol>"
    "<symbol id=\"lock\" viewBox=\"0 0 24 24\"><path d=\"M7 10V7a5 5 0 0 1 10 0v3h1a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1"
    "H6a1 1 0 0 1-1-1v-9a1 1 0 0 1 1-1zm2 0h6V7a3 3 0 0 0-6 0z\"/></symbol>"
    "<symbol id=\"x\" viewBox=\"0 0 24 24\"><path d=\"M6 6l12 12M18 6 6 18\"/></symbol>"
    "<symbol id=\"ok\" viewBox=\"0 0 24 24\"><path d=\"M4 12.5l5 5L20 6.5\"/></symbol>"
    "</svg>";

// A full-screen sheet: close and confirm buttons, the Wi-Fi glyph, a title.
static void sheet_begin(web_page_t *page, const char *id, const char *title_id, const char *title,
                        const char *sub) {
    web_page_append(page,
                    "<dialog id=\"%s\"><div class=\"sheet\"><div class=\"bar\">"
                    "<button class=\"round\" type=\"button\" data-close aria-label=\"Cancel\">"
                    "<svg><use href=\"#x\"/></svg></button>"
                    "<button class=\"round\" type=\"submit\" aria-label=\"Join\"><svg><use href=\"#ok\"/></svg></button>"
                    "</div><svg class=\"hero\"><use href=\"#w3\"/></svg>"
                    "<h2 id=\"%s\">%s</h2><p class=\"sub\">%s</p>",
                    id, title_id, title, sub);
}

static int bars(int8_t rssi) {
    return rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
}

static esp_err_t root_handler(httpd_req_t *req) {
    char query[32] = "";
    bool refresh = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK && strstr(query, "rescan");

    wifi_network_t *networks = calloc(MAX_NETWORKS, sizeof(wifi_network_t));
    web_page_t page;
    if (!networks || !web_page_begin(&page, "AudioKnob setup", "")) {
        free(networks);
        return httpd_resp_send_500(req);
    }
    size_t count = s_ops->scan(networks, MAX_NETWORKS, refresh);

    char name[DEVICE_NAME_MAX + 1];
    device_name_get(name, sizeof(name));
    web_page_append(&page, "%s<main><form id=\"f\" method=\"post\" action=\"/connect\">"
                           "<h1>Set Up</h1>"
                           "<p class=\"label\">Name</p><div class=\"card\"><div class=\"row\">"
                           "<input name=\"name\" maxlength=\"%d\" required aria-label=\"Name\" value=\"",
                    ICONS, DEVICE_NAME_MAX);
    web_page_append_escaped(&page, name);
    web_page_append(&page, "\"></div></div><p class=\"foot\">Shown to AirPlay and Bluetooth devices.</p>"
                           "<p class=\"label\"><span>Choose a Network</span><a href=\"/?rescan=1\">Scan Again</a></p>"
                           "<div class=\"card\">");
    for (size_t i = 0; i < count; i++) {
        web_page_append(&page, "<button class=\"row\" type=\"button\" data-ssid=\"");
        web_page_append_escaped(&page, networks[i].ssid);
        web_page_append(&page, "\"%s><span class=\"n\">", networks[i].secure ? "" : " data-open");
        web_page_append_escaped(&page, networks[i].ssid);
        web_page_append(&page, "</span>%s<svg><use href=\"#w%d\"/></svg></button>",
                        networks[i].secure ? "<svg><use href=\"#lock\"/></svg>" : "", bars(networks[i].rssi));
    }
    web_page_append(&page, "<button class=\"row\" type=\"button\" id=\"other\">Other…</button></div>"
                           "<input type=\"hidden\" name=\"ssid\" id=\"ssid\">");

    sheet_begin(&page, "join", "jointitle", "Join", "Enter the password for this Wi-Fi network.");
    web_page_append(&page, "<div class=\"pill\"><input type=\"password\" name=\"password\" placeholder=\"Password\" "
                           "maxlength=\"64\" disabled aria-label=\"Password\"></div></div></dialog>");
    sheet_begin(&page, "othersheet", "othertitle", "Other Network", "Enter the network name and password.");
    web_page_append(&page, "<div class=\"pill\"><input name=\"other\" placeholder=\"Network name\" maxlength=\"32\" "
                           "required disabled autocapitalize=\"none\" autocorrect=\"off\" aria-label=\"Network name\">"
                           "</div><div class=\"pill\"><input type=\"password\" name=\"password\" placeholder=\"Password\" "
                           "maxlength=\"64\" disabled aria-label=\"Password\"></div>"
                           "<p class=\"foot\">Leave the password empty for an open network.</p></div></dialog>"
                           "</form></main>");
    // Only the open sheet's fields are enabled, so only they are submitted.
    web_page_append(&page,
                    "<script>"
                    "const f=document.getElementById('f');"
                    "function sheet(id){const d=document.getElementById(id);"
                    "d.querySelectorAll('input').forEach(i=>i.disabled=false);d.showModal();d.querySelector('input').focus()}"
                    "document.querySelectorAll('dialog').forEach(d=>{"
                    "d.onclose=()=>d.querySelectorAll('input').forEach(i=>{i.disabled=true;i.value=''});"
                    "d.querySelector('[data-close]').onclick=()=>d.close()});"
                    "document.querySelectorAll('[data-ssid]').forEach(b=>b.onclick=()=>{"
                    "f.ssid.value=b.dataset.ssid;"
                    "if('open' in b.dataset){f.requestSubmit();return}"
                    "document.getElementById('jointitle').textContent='Join \\u201c'+b.dataset.ssid+'\\u201d';"
                    "sheet('join')});"
                    "document.getElementById('other').onclick=()=>{f.ssid.value='';sheet('othersheet')};"
                    "</script>");

    free(networks);
    return web_page_send(req, &page);
}

static esp_err_t connect_handler(httpd_req_t *req) {
    char form[FORM_MAX + 1];
    if (!web_read_form(req, form, sizeof(form))) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad form");
    }

    char name[DEVICE_NAME_MAX * 3 + 1], ssid[33 * 3], other[33 * 3], password[65 * 3];
    web_form_value(form, "name", name, sizeof(name));
    web_form_value(form, "ssid", ssid, sizeof(ssid));
    web_form_value(form, "other", other, sizeof(other));
    web_form_value(form, "password", password, sizeof(password));

    const char *chosen = other[0] ? other : ssid;
    if (!chosen[0] || strlen(chosen) > 32 || strlen(password) > 64) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid network name or password");
    }
    // The setup hotspot keeps its SSID until the restart that follows a
    // successful connection, which then advertises the new name.
    if (name[0] && !device_name_set(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid device name");
    }
    s_ops->submit(chosen, password);
    return web_redirect(req, "/status");
}

static esp_err_t status_handler(httpd_req_t *req) {
    web_page_t page;
    if (!web_page_begin(&page, "AudioKnob setup", "<meta http-equiv=\"refresh\" content=\"2\">")) {
        return httpd_resp_send_500(req);
    }

    char status[96];
    s_ops->status(status, sizeof(status));

    char name[DEVICE_NAME_MAX + 1];
    device_name_get(name, sizeof(name));
    web_page_append(&page, "<main><h1>");
    web_page_append_escaped(&page, name);
    web_page_append(&page, "</h1><div class=\"card\"><div class=\"row\"><span class=\"n\">");
    web_page_append_escaped(&page, status);
    web_page_append(&page, "</span></div></div><p class=\"foot\">The device restarts once it connects, and this "
                           "setup network closes.</p><p class=\"foot\"><a href=\"/\">Choose another network</a></p>"
                           "</main>");
    return web_page_send(req, &page);
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
