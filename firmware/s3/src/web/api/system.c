#include "web/api/api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio/mic/mic.h"
#include "drivers/uart_link.h"
#include "esp_heap_caps.h"
#include "update/github_release.h"
#include "update/ota.h"
#include "web/page.h"

// httpd_req_recv() gives up after the server's 5 s receive timeout; a
// phone that pauses mid-upload gets a few of those.
#define UPLOAD_RETRIES 3

// Debug: records ?s= seconds (1-30) from the mic as a 16 kHz mono WAV.
// ?raw=1 bypasses processing; ?gain=0..36 and ?ns=0 tune voice processing.
// ?slot=r reads the other PDM clock edge.
esp_err_t web_api_mic_wav(httpd_req_t *req) {
    enum { HEADER = 44, MAX_SECONDS = 30 };
    char query[96] = "", value[8];
    httpd_req_get_url_query_str(req, query, sizeof(query));
    int seconds = 5;
    if (httpd_query_key_value(query, "s", value, sizeof(value)) == ESP_OK && !web_parse_int(value, 1, MAX_SECONDS, &seconds)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "s must be 1-30");
    }
    bool raw = httpd_query_key_value(query, "raw", value, sizeof(value)) == ESP_OK && !strcmp(value, "1");
    bool right = httpd_query_key_value(query, "slot", value, sizeof(value)) == ESP_OK && !strcmp(value, "r");

    int gain = MIC_DEFAULT_GAIN_DB;
    if (httpd_query_key_value(query, "gain", value, sizeof(value)) == ESP_OK &&
        !web_parse_int(value, 0, MIC_MAX_GAIN_DB, &gain)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "gain must be 0-36 dB");
    }
    int noise_level = MIC_DEFAULT_NOISE_LEVEL;
    if (httpd_query_key_value(query, "ns", value, sizeof(value)) == ESP_OK) {
        if (!web_parse_int(value, 0, MIC_MAX_NOISE_LEVEL, &noise_level)) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ns must be 0-3");
        }
    }
    size_t frames = (size_t)seconds * MIC_SAMPLE_RATE;
    uint32_t data_bytes = frames * sizeof(int16_t);
    uint8_t *wav = heap_caps_malloc(HEADER + data_bytes, MALLOC_CAP_SPIRAM);
    if (!wav) {
        return httpd_resp_send_500(req);
    }
    const mic_config_t config = {.gain_db = (uint8_t)gain, .noise_level = (uint8_t)noise_level,
                                 .raw = raw, .right_slot = right};
    if (!mic_start(&config)) {
        free(wav);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Mic busy or processing memory unavailable");
    }
    int16_t *pcm = (int16_t *)(wav + HEADER);
    size_t got = 0;
    while (got < frames) {
        size_t n = mic_read(pcm + got, frames - got, 1000);
        if (!n) {
            break;
        }
        got += n;
    }
    mic_stop();
    data_bytes = got * sizeof(int16_t);

    const uint32_t byte_rate = MIC_SAMPLE_RATE * sizeof(int16_t);
    const uint32_t fields[] = {
        36 + data_bytes, 0x45564157 /* WAVE */, 0x20746d66 /* fmt  */, 16,
        1 | 1 << 16 /* PCM, mono */, MIC_SAMPLE_RATE, byte_rate, 2 | 16 << 16 /* align, bits */,
        0x61746164 /* data */, data_bytes,
    };
    memcpy(wav, "RIFF", 4);
    memcpy(wav + 4, fields, sizeof(fields));
    char cost[16];
    snprintf(cost, sizeof(cost), "%.2f", (double)mic_processing_cpu_pct());
    httpd_resp_set_hdr(req, "X-Mic-CPU-Percent", cost);
    httpd_resp_set_type(req, "audio/wav");
    esp_err_t err = httpd_resp_send(req, (const char *)wav, HEADER + data_bytes);
    free(wav);
    return err;
}

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
