#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"

// Small HTML page builder shared by the setup portal and the LAN settings
// page: one PSRAM buffer, printf-style appends, one dark stylesheet.

typedef struct {
    char *buf;
    size_t len;
} web_page_t;

// Allocates the buffer and writes <head> plus the opening <body>.
// head_extra (e.g. a meta refresh) goes inside <head>. False on OOM.
bool web_page_begin(web_page_t *page, const char *title, const char *head_extra);

void web_page_append(web_page_t *page, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// For anything user- or network-supplied (SSIDs, the device name).
void web_page_append_escaped(web_page_t *page, const char *text);

// Closes the document, sends it and frees the buffer.
esp_err_t web_page_send(httpd_req_t *req, web_page_t *page);

// Reads an application/x-www-form-urlencoded body (truncated to len - 1).
bool web_read_form(httpd_req_t *req, char *form, size_t len);

// Looks up key in a form body and URL-decodes it. "" when absent.
void web_form_value(const char *form, const char *key, char *out, size_t len);

// 303 to location, the usual reply to a form POST.
esp_err_t web_redirect(httpd_req_t *req, const char *location);
