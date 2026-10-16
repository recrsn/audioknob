#include "stream_parse.h"

#include <ctype.h>
#include <string.h>
#include <strings.h>

static const struct {
    const char *type;
    stream_format_t format;
} k_types[] = {
    {"audio/mpeg", STREAM_FORMAT_MP3},
    {"audio/mp3", STREAM_FORMAT_MP3},
    {"audio/mpeg3", STREAM_FORMAT_MP3},
    {"audio/x-mpeg", STREAM_FORMAT_MP3},
    {"audio/aac", STREAM_FORMAT_AAC},
    {"audio/aacp", STREAM_FORMAT_AAC},
    {"audio/x-aac", STREAM_FORMAT_AAC},
    {"audio/x-aacp", STREAM_FORMAT_AAC},
    {"audio/x-scpls", STREAM_FORMAT_PLS},
    {"audio/scpls", STREAM_FORMAT_PLS},
    {"application/pls+xml", STREAM_FORMAT_PLS},
    {"audio/x-mpegurl", STREAM_FORMAT_M3U},
    {"audio/mpegurl", STREAM_FORMAT_M3U},
    {"application/x-mpegurl", STREAM_FORMAT_M3U},
    {"application/vnd.apple.mpegurl", STREAM_FORMAT_M3U},
};

static const struct {
    const char *ext;
    stream_format_t format;
} k_exts[] = {
    {".mp3", STREAM_FORMAT_MP3}, {".aac", STREAM_FORMAT_AAC}, {".aacp", STREAM_FORMAT_AAC},
    {".pls", STREAM_FORMAT_PLS}, {".m3u", STREAM_FORMAT_M3U}, {".m3u8", STREAM_FORMAT_M3U},
};

static stream_format_t from_extension(const char *url) {
    size_t end = strcspn(url, "?#");
    for (size_t i = 0; i < sizeof(k_exts) / sizeof(k_exts[0]); i++) {
        size_t n = strlen(k_exts[i].ext);
        if (end >= n && !strncasecmp(url + end - n, k_exts[i].ext, n)) {
            return k_exts[i].format;
        }
    }
    return STREAM_FORMAT_UNKNOWN;
}

stream_format_t stream_format_detect(const char *content_type, const char *url) {
    if (content_type) {
        while (*content_type == ' ') {
            content_type++;
        }
        size_t len = strcspn(content_type, "; ");
        for (size_t i = 0; i < sizeof(k_types) / sizeof(k_types[0]); i++) {
            if (strlen(k_types[i].type) == len && !strncasecmp(content_type, k_types[i].type, len)) {
                return k_types[i].format;
            }
        }
    }
    // Servers often label streams application/octet-stream or text/plain.
    return url ? from_extension(url) : STREAM_FORMAT_UNKNOWN;
}

stream_format_t stream_format_sniff(const uint8_t *data, size_t len) {
    if (len >= 3 && !memcmp(data, "ID3", 3)) {
        return STREAM_FORMAT_MP3;
    }
    if (len >= 10 && !strncasecmp((const char *)data, "[playlist]", 10)) {
        return STREAM_FORMAT_PLS;
    }
    if (len >= 7 && (!memcmp(data, "#EXTM3U", 7) || !strncasecmp((const char *)data, "http", 4))) {
        return STREAM_FORMAT_M3U;
    }
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] != 0xFF || (data[i + 1] & 0xE0) != 0xE0) {
            continue;
        }
        // ADTS is the 12-bit sync with layer 00; MPEG audio has layer 1-3.
        if ((data[i + 1] & 0xF6) == 0xF0) {
            return STREAM_FORMAT_AAC;
        }
        if ((data[i + 1] & 0x06) != 0) {
            return STREAM_FORMAT_MP3;
        }
    }
    return STREAM_FORMAT_UNKNOWN;
}

static bool is_stream_url(const char *s) {
    return !strncasecmp(s, "http://", 7) || !strncasecmp(s, "https://", 8);
}

// Copies the line at `s` (to CR/LF/NUL), trimmed, into out.
static void copy_line(const char *s, char *out, size_t len) {
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    size_t n = strcspn(s, "\r\n");
    while (n && isspace((unsigned char)s[n - 1])) {
        n--;
    }
    if (n >= len) {
        n = len - 1;
    }
    memcpy(out, s, n);
    out[n] = '\0';
}

bool stream_playlist_first_url(stream_format_t format, const char *body, char *url, size_t len, bool *hls) {
    *hls = format == STREAM_FORMAT_M3U && strstr(body, "#EXT-X-") != NULL;
    if (*hls) {
        return false;
    }
    for (const char *line = body; *line; line += strcspn(line, "\n"), line += *line == '\n') {
        while (*line == ' ' || *line == '\t' || *line == '\r') {
            line++;
        }
        const char *value = line;
        if (format == STREAM_FORMAT_PLS) {
            if (strncasecmp(line, "File", 4)) {
                continue;
            }
            value = strchr(line, '=');
            if (!value || value > line + strcspn(line, "\r\n")) {
                continue;
            }
            value++;
        } else if (*line == '#') {
            continue;
        }
        copy_line(value, url, len);
        if (is_stream_url(url)) {
            return true;
        }
    }
    url[0] = '\0';
    return false;
}

static bool valid_utf8(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        size_t extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 4;
        if (extra == 4 || (extra && i + extra >= n)) {
            return false;
        }
        for (size_t k = 1; k <= extra; k++) {
            if ((s[i + k] & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

void stream_text_utf8(const char *in, size_t n, char *out, size_t len) {
    size_t o = 0;
    if (valid_utf8((const unsigned char *)in, n)) {
        if (n >= len) {
            n = len - 1;
            while (n && ((unsigned char)in[n] & 0xC0) == 0x80) {
                n--;
            }
        }
        memcpy(out, in, n);
        o = n;
    } else {
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)in[i];
            if (o + (c < 0x80 ? 1 : 2) >= len) {
                break;
            }
            if (c < 0x80) {
                out[o++] = (char)c;
            } else {
                out[o++] = (char)(0xC0 | c >> 6);
                out[o++] = (char)(0x80 | (c & 0x3F));
            }
        }
    }
    out[o] = '\0';
}

bool stream_icy_title(const char *meta, size_t len, char *out, size_t out_len) {
    static const char key[] = "StreamTitle='";
    const char *end = meta + len;
    const char *start = NULL;
    for (const char *p = meta; p + sizeof(key) - 1 <= end; p++) {
        if (!memcmp(p, key, sizeof(key) - 1)) {
            start = p + sizeof(key) - 1;
            break;
        }
    }
    if (!start) {
        return false;
    }
    // Titles may hold apostrophes; the value ends at "';" (or the block).
    const char *stop = start;
    while (stop < end && *stop && !(stop[0] == '\'' && (stop + 1 == end || stop[1] == ';' || stop[1] == '\0'))) {
        stop++;
    }
    while (start < stop && *start == ' ') {
        start++;
    }
    while (stop > start && stop[-1] == ' ') {
        stop--;
    }

    stream_text_utf8(start, (size_t)(stop - start), out, out_len);
    return out[0] != '\0';
}
