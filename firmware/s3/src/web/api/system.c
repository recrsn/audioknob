#include "web/api/api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/uart_link.h"
#include "update/github_release.h"
#include "update/ota.h"
#include "web/page.h"

// httpd_req_recv() gives up after the server's 5 s receive timeout; a
// phone that pauses mid-upload gets a few of those.
#define UPLOAD_RETRIES 3

// Opens the pairing window, or forgets every paired device. The code
// check itself stays on the device's screen.
esp_err_t web_api_pairing(httpd_req_t *req) {
    char form[WEB_FORM_MAX + 1], action[8];
    if (!web_read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    pairing_status_t pairing;
    if (!uart_link_get_pairing(&pairing)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bluetooth isn't on");
    }
    web_form_value(form, "action", action, sizeof(action));
    if (!strcmp(action, "open")) {
        uart_link_send_command(CMD_PAIRING_OPEN);
    } else if (!strcmp(action, "forget")) {
        uart_link_send_command(CMD_FORGET_DEVICES);
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown action");
    }
    return web_send_ok(req);
}

static int read_upload(void *ctx, char *buf, size_t len) {
    for (int tries = 0; tries < UPLOAD_RETRIES; tries++) {
        int n = httpd_req_recv(ctx, buf, len);
        if (n != HTTPD_SOCK_ERR_TIMEOUT) {
            return n;
        }
    }
    return -1;
}

// The raw image as the request body. It only lands in the spare slot: the
// device asks before booting it.
esp_err_t web_api_update(httpd_req_t *req) {
    if (!web_same_origin(req)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Cross-site request refused");
    }
    if (ota_receive(req->content_len, read_upload, req)) {
        return web_send_ok(req);
    }
    ota_status_t ota;
    ota_get_status(&ota);
    // No error of its own when an install is running: that one's status stays.
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, ota.error[0] ? ota.error : "Another update is pending or in progress");
    // Closes the socket instead of draining what's left of a refused
    // image; the browser may see that as a network error, so the page
    // reads the reason from /api/state.
    return ESP_FAIL;
}

esp_err_t web_api_release(httpd_req_t *req) {
    char form[WEB_FORM_MAX + 1], action[16], target[16];
    if (!web_read_post(req, form, sizeof(form))) {
        return ESP_OK;
    }
    web_form_value(form, "action", action, sizeof(action));
    web_form_value(form, "target", target, sizeof(target));
    bool ok;
    if (!strcmp(action, "check")) {
        ok = github_release_check();
    } else if (!strcmp(action, "download") && (!strcmp(target, "s3") || !strcmp(target, "bluetooth"))) {
        ok = github_release_download(!strcmp(target, "s3") ? OTA_TARGET_S3 : OTA_TARGET_ESP32);
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown release action or target");
    }
    return ok ? web_send_ok(req) : httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
        "Update busy or unavailable; check for updates first");
}
