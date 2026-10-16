#include "stream_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#define CONNECT_TIMEOUT_MS 10000
// Reads wake this often to check should_stop().
#define READ_TIMEOUT_MS 500
#define STALL_US (15 * 1000 * 1000LL)
#define MAX_HOPS 8
#define READ_CHUNK 4096
#define TEXT_MAX 8192 // a playlist body, or one ICY metadata block (<= 4080)
#define RX_BUFFER 2048
#define TX_BUFFER 1024
#define RING_WRITE_WAIT pdMS_TO_TICKS(200)

static const char *TAG = "stream_net";

typedef struct {
    char content_type[64];
    char location[STREAM_URL_MAX];
    char icy_name[64];
    int metaint;
    int bitrate;
} headers_t;

// Too big for the task's stack; lives in PSRAM for one stream_net_run().
typedef struct {
    headers_t hdr;
    char url[STREAM_URL_MAX];
    char next[STREAM_URL_MAX];
    uint8_t buf[READ_CHUNK];
    char text[TEXT_MAX];
    size_t text_len;
    // ICY demux: audio bytes until the next length byte, then that many
    // metadata bytes.
    int until_meta;
    int meta_left;
} conn_t;

typedef enum {
    READ_DATA,
    READ_EOF,
    READ_STALLED,
    READ_STOPPED,
} read_status_t;

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
    if (evt->event_id != HTTP_EVENT_ON_HEADER) {
        return ESP_OK;
    }
    headers_t *h = evt->user_data;
    const char *key = evt->header_key, *value = evt->header_value;
    if (!strcasecmp(key, "Content-Type")) {
        snprintf(h->content_type, sizeof(h->content_type), "%s", value);
    } else if (!strcasecmp(key, "Location")) {
        snprintf(h->location, sizeof(h->location), "%s", value);
    } else if (!strcasecmp(key, "icy-metaint")) {
        h->metaint = atoi(value);
    } else if (!strcasecmp(key, "icy-name")) {
        stream_text_utf8(value, strlen(value), h->icy_name, sizeof(h->icy_name));
    } else if (!strcasecmp(key, "icy-br")) {
        h->bitrate = atoi(value);
    }
    return ESP_OK;
}

// A Location header against the URL it came from: absolute, scheme-relative
// or path-relative.
static void resolve_location(const char *base, const char *loc, char *out, size_t len) {
    if (strstr(loc, "://")) {
        snprintf(out, len, "%s", loc);
        return;
    }
    const char *host = strstr(base, "://");
    host = host ? host + 3 : base;
    if (loc[0] == '/' && loc[1] == '/') {
        snprintf(out, len, "%.*s%s", (int)(host - base - 2), base, loc);
    } else if (loc[0] == '/') {
        snprintf(out, len, "%.*s%s", (int)(strcspn(host, "/?#") + (size_t)(host - base)), base, loc);
    } else {
        const char *slash = strrchr(host, '/');
        size_t keep = slash ? (size_t)(slash - base + 1) : strlen(base);
        snprintf(out, len, "%.*s%s%s", (int)keep, base, slash ? "" : "/", loc);
    }
}

static esp_http_client_handle_t open_client(conn_t *c) {
    memset(&c->hdr, 0, sizeof(c->hdr));
    const esp_http_client_config_t cfg = {
        .url = c->url,
        .timeout_ms = CONNECT_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "AudioKnob/1.0",
        .event_handler = on_http_event,
        .user_data = &c->hdr,
        .buffer_size = RX_BUFFER,
        .buffer_size_tx = TX_BUFFER,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client) {
        esp_http_client_set_header(client, "Icy-MetaData", "1");
        esp_http_client_set_header(client, "Accept", "*/*");
    }
    return client;
}

static read_status_t read_some(esp_http_client_handle_t client, conn_t *c, const stream_net_ops_t *ops, int *len) {
    int64_t since = esp_timer_get_time();
    for (;;) {
        if (ops->should_stop(ops->ctx)) {
            return READ_STOPPED;
        }
        int n = esp_http_client_read(client, (char *)c->buf, sizeof(c->buf));
        if (n > 0) {
            *len = n;
            return READ_DATA;
        }
        if (n != -ESP_ERR_HTTP_EAGAIN) {
            return READ_EOF;
        }
        if (esp_timer_get_time() - since > STALL_US) {
            return READ_STALLED;
        }
    }
}

static void meta_done(conn_t *c, const stream_net_ops_t *ops) {
    char title[128];
    if (!stream_icy_title(c->text, c->text_len, title, sizeof(title))) {
        title[0] = '\0';
    }
    ops->on_title(ops->ctx, title);
}

// Strips ICY metadata out of buf in place; returns the audio bytes left.
static size_t icy_demux(conn_t *c, size_t len, const stream_net_ops_t *ops) {
    if (c->hdr.metaint <= 0) {
        return len;
    }
    uint8_t *buf = c->buf;
    size_t out = 0;
    for (size_t i = 0; i < len;) {
        if (c->meta_left > 0) {
            size_t n = len - i < (size_t)c->meta_left ? len - i : (size_t)c->meta_left;
            memcpy(c->text + c->text_len, buf + i, n);
            c->text_len += n;
            c->meta_left -= (int)n;
            i += n;
            if (c->meta_left == 0) {
                meta_done(c, ops);
                c->until_meta = c->hdr.metaint;
            }
        } else if (c->until_meta == 0) {
            c->meta_left = buf[i++] * 16;
            c->text_len = 0;
            if (c->meta_left == 0) {
                c->until_meta = c->hdr.metaint;
            }
        } else {
            size_t n = len - i < (size_t)c->until_meta ? len - i : (size_t)c->until_meta;
            memmove(buf + out, buf + i, n);
            out += n;
            i += n;
            c->until_meta -= (int)n;
        }
    }
    return out;
}

static bool write_audio(stream_ring_t *ring, const uint8_t *data, size_t len, const stream_net_ops_t *ops) {
    size_t done = 0;
    while (done < len) {
        if (ops->should_stop(ops->ctx)) {
            return false;
        }
        done += stream_ring_write(ring, data + done, len - done, RING_WRITE_WAIT);
    }
    return true;
}

static stream_net_result_t pump(esp_http_client_handle_t client, conn_t *c, size_t first, stream_ring_t *ring,
                                const stream_net_ops_t *ops, char *error, size_t error_len) {
    size_t n = first;
    for (;;) {
        if (!write_audio(ring, c->buf, n, ops)) {
            return STREAM_NET_STOPPED;
        }
        int len = 0;
        switch (read_some(client, c, ops, &len)) {
        case READ_STOPPED:
            return STREAM_NET_STOPPED;
        case READ_STALLED:
            snprintf(error, error_len, "Stream stalled");
            return STREAM_NET_RETRY;
        case READ_EOF:
            snprintf(error, error_len, "Stream ended");
            return STREAM_NET_RETRY;
        case READ_DATA:
            n = icy_demux(c, (size_t)len, ops);
            break;
        }
    }
}

// Reads the rest of a playlist into c->text (the first chunk is in c->buf)
// and picks its first entry as the next URL.
static stream_net_result_t follow_playlist(esp_http_client_handle_t client, conn_t *c, stream_format_t format,
                                           int first, const stream_net_ops_t *ops, char *error, size_t error_len) {
    c->text_len = 0;
    int len = first;
    for (;;) {
        size_t room = sizeof(c->text) - 1 - c->text_len;
        size_t n = (size_t)len < room ? (size_t)len : room;
        memcpy(c->text + c->text_len, c->buf, n);
        c->text_len += n;
        if (!room) {
            break;
        }
        read_status_t st = read_some(client, c, ops, &len);
        if (st == READ_STOPPED) {
            return STREAM_NET_STOPPED;
        }
        if (st != READ_DATA) {
            break;
        }
    }
    c->text[c->text_len] = '\0';
    bool hls;
    if (stream_playlist_first_url(format, c->text, c->next, sizeof(c->next), &hls)) {
        return STREAM_NET_RETRY; // i.e. go on to c->next
    }
    snprintf(error, error_len, hls ? "HLS streams aren't supported" : "Empty playlist");
    return STREAM_NET_FAILED;
}

// One request. RETRY with c->next set means "go on there".
static stream_net_result_t hop(conn_t *c, stream_ring_t *ring, const stream_net_ops_t *ops, char *error,
                               size_t error_len) {
    c->next[0] = '\0';
    esp_http_client_handle_t client = open_client(c);
    if (!client) {
        snprintf(error, error_len, "Bad station address");
        return STREAM_NET_FAILED;
    }
    stream_net_result_t result = STREAM_NET_RETRY;
    if (esp_http_client_open(client, 0) != ESP_OK) {
        snprintf(error, error_len, "Can't reach the station");
        goto done;
    }
    if (esp_http_client_fetch_headers(client) < 0) {
        snprintf(error, error_len, "No answer from the station");
        goto done;
    }
    int status = esp_http_client_get_status_code(client);
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        if (c->hdr.location[0]) {
            resolve_location(c->url, c->hdr.location, c->next, sizeof(c->next));
        } else {
            snprintf(error, error_len, "Bad redirect");
            result = STREAM_NET_FAILED;
        }
        goto done;
    }
    if (status != 200) {
        snprintf(error, error_len, "Station answered %d", status);
        // Not found or refused won't change by retrying; overload might.
        bool transient = status >= 500 || status == 408 || status == 429;
        result = transient ? STREAM_NET_RETRY : STREAM_NET_FAILED;
        goto done;
    }
    esp_http_client_set_timeout_ms(client, READ_TIMEOUT_MS);

    int len = 0;
    read_status_t st = read_some(client, c, ops, &len);
    if (st == READ_STOPPED) {
        result = STREAM_NET_STOPPED;
        goto done;
    }
    if (st != READ_DATA) {
        snprintf(error, error_len, "No audio from the station");
        goto done;
    }

    stream_format_t format = stream_format_detect(c->hdr.content_type, c->url);
    if (format == STREAM_FORMAT_UNKNOWN) {
        stream_format_t sniffed = stream_format_sniff(c->buf, (size_t)len);
        format = sniffed == STREAM_FORMAT_PLS || sniffed == STREAM_FORMAT_M3U ? sniffed : format;
    }
    if (format == STREAM_FORMAT_PLS || format == STREAM_FORMAT_M3U) {
        result = follow_playlist(client, c, format, len, ops, error, error_len);
        goto done;
    }

    c->until_meta = c->hdr.metaint;
    c->meta_left = 0;
    size_t audio = icy_demux(c, (size_t)len, ops);
    if (format == STREAM_FORMAT_UNKNOWN) {
        format = stream_format_sniff(c->buf, audio);
    }
    if (format != STREAM_FORMAT_MP3 && format != STREAM_FORMAT_AAC) {
        ESP_LOGW(TAG, "unsupported stream: \"%s\"", c->hdr.content_type);
        snprintf(error, error_len, "Unsupported stream format");
        result = STREAM_NET_FAILED;
        goto done;
    }
    stream_info_t info = {.format = format, .bitrate_kbps = c->hdr.bitrate};
    snprintf(info.name, sizeof(info.name), "%s", c->hdr.icy_name);
    ESP_LOGI(TAG, "%s: %s, %d kbps, metaint %d", c->url, format == STREAM_FORMAT_MP3 ? "MP3" : "AAC",
             c->hdr.bitrate, c->hdr.metaint);
    ops->on_audio(ops->ctx, &info);
    result = pump(client, c, audio, ring, ops, error, error_len);

done:
    esp_http_client_cleanup(client);
    return result;
}

stream_net_result_t stream_net_run(const char *url, stream_ring_t *ring, const stream_net_ops_t *ops, char *error,
                                   size_t error_len) {
    conn_t *c = heap_caps_calloc(1, sizeof(*c), MALLOC_CAP_SPIRAM);
    if (!c) {
        snprintf(error, error_len, "Out of memory");
        return STREAM_NET_RETRY;
    }
    snprintf(c->url, sizeof(c->url), "%s", url);
    stream_net_result_t result = STREAM_NET_FAILED;
    snprintf(error, error_len, "Too many redirects");
    for (int i = 0; i < MAX_HOPS && !ops->should_stop(ops->ctx); i++) {
        result = hop(c, ring, ops, error, error_len);
        if (result != STREAM_NET_RETRY || !c->next[0]) {
            break;
        }
        ESP_LOGI(TAG, "%s -> %s", c->url, c->next);
        memcpy(c->url, c->next, sizeof(c->url));
        result = STREAM_NET_FAILED;
        snprintf(error, error_len, "Too many redirects");
    }
    if (ops->should_stop(ops->ctx)) {
        result = STREAM_NET_STOPPED;
    }
    heap_caps_free(c);
    return result;
}
