#include "web_page.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

#define PAGE_SIZE 8192 // PSRAM: these pages aren't speed-critical

bool web_page_begin(web_page_t *page, const char *title, const char *head_extra) {
    page->buf = heap_caps_malloc(PAGE_SIZE, MALLOC_CAP_SPIRAM);
    page->len = 0;
    if (!page->buf) {
        return false;
    }
    page->buf[0] = '\0';
    web_page_append(page,
                    "<!doctype html><html><head><meta charset=\"utf-8\">"
                    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>");
    web_page_append_escaped(page, title);
    web_page_append(page,
                    "</title>%s<style>"
                    "body{font-family:-apple-system,system-ui,sans-serif;background:#0b0b10;color:#eee;"
                    "max-width:420px;margin:0 auto;padding:24px 16px}"
                    "h1{font-size:22px;margin:0 0 16px}label{display:block;margin:14px 0 6px;color:#aaa}"
                    "select,input{width:100%%;box-sizing:border-box;padding:12px;font-size:16px;border-radius:10px;"
                    "border:1px solid #333;background:#16161e;color:#eee}"
                    "button{width:100%%;margin-top:20px;padding:14px;font-size:17px;border:0;border-radius:10px;"
                    "background:#4ea1ff;color:#fff}a{color:#4ea1ff}p{line-height:1.5}"
                    ".hint{margin:6px 0 0;font-size:14px;color:#888}.ok{color:#6fd08c}.err{color:#ff7a7a}"
                    "</style></head><body>",
                    head_extra);
    return true;
}

void web_page_append(web_page_t *page, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(page->buf + page->len, PAGE_SIZE - page->len, fmt, args);
    va_end(args);
    if (n > 0) {
        page->len += (size_t)n < PAGE_SIZE - page->len ? (size_t)n : PAGE_SIZE - page->len - 1;
    }
}

void web_page_append_escaped(web_page_t *page, const char *text) {
    for (; *text && page->len < PAGE_SIZE - 8; text++) {
        switch (*text) {
        case '<': web_page_append(page, "&lt;"); break;
        case '>': web_page_append(page, "&gt;"); break;
        case '&': web_page_append(page, "&amp;"); break;
        case '"': web_page_append(page, "&quot;"); break;
        case '\'': web_page_append(page, "&#39;"); break;
        default: page->buf[page->len++] = *text; page->buf[page->len] = '\0'; break;
        }
    }
}

esp_err_t web_page_send(httpd_req_t *req, web_page_t *page) {
    web_page_append(page, "</body></html>");
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, page->buf, page->len);
    free(page->buf);
    page->buf = NULL;
    return err;
}

bool web_read_form(httpd_req_t *req, char *form, size_t len) {
    size_t total = 0;
    while (total < req->content_len && total < len - 1) {
        int n = httpd_req_recv(req, form + total, len - 1 - total);
        if (n <= 0) {
            return false;
        }
        total += n;
    }
    form[total] = '\0';
    return true;
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

void web_form_value(const char *form, const char *key, char *out, size_t len) {
    if (httpd_query_key_value(form, key, out, len) != ESP_OK) {
        out[0] = '\0';
        return;
    }
    url_decode(out);
}

esp_err_t web_redirect(httpd_req_t *req, const char *location) {
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_send(req, NULL, 0);
}
