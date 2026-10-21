#include "web_page.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"

#define PAGE_SIZE 16384 // PSRAM: these pages aren't speed-critical

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
                    ":root{color-scheme:dark}"
                    "body{margin:0;background:#000;color:#f2f2f7;font:17px/1.35 -apple-system,system-ui,sans-serif;"
                    "-webkit-font-smoothing:antialiased}"
                    "main{max-width:560px;margin:0 auto;padding:0 16px 48px}"
                    "h1{font-size:34px;margin:0;padding:48px 4px 16px}"
                    "a,.blue{color:#0a84ff;text-decoration:none}"
                    ".label{display:flex;justify-content:space-between;font-size:13px;text-transform:uppercase;"
                    "color:#8e8e93;margin:24px 16px 6px}.label a{text-transform:none;font-size:15px}"
                    ".card{background:#1c1c1e;border-radius:12px;overflow:hidden}"
                    ".row{position:relative;box-sizing:border-box;display:flex;align-items:center;gap:12px;width:100%%;"
                    "min-height:46px;padding:8px 16px;border:0;background:none;color:inherit;font:inherit;text-align:left}"
                    "button.row{cursor:pointer}"
                    ".row+.row::before{content:\"\";position:absolute;top:0;left:16px;right:0;border-top:.5px solid #38383a}"
                    ".row .n{flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}"
                    ".row svg{flex:none;width:18px;height:18px;fill:currentColor}"
                    ".foot{font-size:13px;color:#8e8e93;margin:6px 16px 0}"
                    "input{box-sizing:border-box;width:100%%;font:inherit;color:inherit;background:none;border:0;outline:none;"
                    "padding:6px 0}"
                    ".ok{color:#30d158}.err{color:#ff453a}"
                    "dialog{box-sizing:border-box;width:100%%;height:100%%;max-width:none;max-height:none;margin:0;padding:0;"
                    "border:0;background:#1c1c1e;color:inherit}"
                    "dialog::backdrop{background:rgba(0,0,0,.5)}"
                    "@media(min-width:700px){dialog{width:560px;height:auto;max-height:90vh;margin:auto;border-radius:20px}}"
                    ".sheet{max-width:560px;margin:0 auto;padding:16px 24px 32px}"
                    ".bar{display:flex;justify-content:space-between}"
                    ".round{width:44px;height:44px;border-radius:50%%;border:0;padding:0;background:#3a3a3c;color:#fff;"
                    "display:grid;place-items:center;cursor:pointer;"
                    "background-image:linear-gradient(rgba(255,255,255,.12),rgba(255,255,255,0));"
                    "box-shadow:inset 0 1px 0 rgba(255,255,255,.2),0 1px 3px rgba(0,0,0,.5)}"
                    ".round:active{transform:scale(.94)}.round:disabled{opacity:.4}"
                    ".round svg{width:20px;height:20px;fill:none;stroke:currentColor;stroke-width:2.4;stroke-linecap:round;"
                    "stroke-linejoin:round}"
                    ".hero{display:block;width:76px;height:76px;margin:36px auto 32px;fill:#0a84ff}"
                    "h2{font-size:24px;margin:0}.sub{font-size:20px;color:#8e8e93;margin:4px 0 24px}"
                    ".pill{background:#2c2c2e;border-radius:26px;padding:12px 20px;margin-bottom:10px}"
                    ".sheet .foot{margin:6px 20px 0}"
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
