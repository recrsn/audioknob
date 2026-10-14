/*
 * AirPlay 2 receiver behind raop.h, built after shairport-sync
 * (identity, Bonjour records, feature flags: shairport.c and
 * bonjour_strings.c, Copyright (c) James Laird 2013, Mike Brady
 * 2014--2026, MIT License) with HomeKit pairing from owntone's pair_ap and
 * our own PTP follower. No FairPlay: senders pair (transient HomeKit) instead.
 * AirPlay 1 senders use the same RTSP port, as with shairport-sync (ap1.c).
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "ap2.h"
#include "dacp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "mdns.h"
#include "pair.h"
#include "ptp.h"
#include "raop_setup.h"
#include "rtsp.h"
#include "session.h"

#define NAME_MAX_LEN 64
#define INITIAL_VOLUME_DB (-15.0f)

// shairport-sync's AirPlay 2 feature set: audio, buffered audio, PTP, HomeKit
// transient pairing, no FairPlay-only paths.
#define FEATURES_BASE 0x00010340401C4A00ULL
// Needed to be added to the Home app (46: SupportsHKPairingAndAccessControl).
#define FEATURES_HOMEKIT ((1ULL << 46) | (1ULL << 47))
// Classic metadata over SET_PARAMETER: artwork (15), progress (16), text (17).
#define FEATURES_METADATA ((1ULL << 15) | (1ULL << 16) | (1ULL << 17))
#define FEATURES (FEATURES_BASE | FEATURES_HOMEKIT | FEATURES_METADATA)

struct raop_ctx_s {
    raop_event_cb_t event_cb;
    raop_pcm_cb_t pcm_cb;
};

static const char *TAG = "ap2";

static raop_ctx_t s_ctx;
static char s_name[NAME_MAX_LEN + 1];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static float s_volume_db = INITIAL_VOLUME_DB;
static uint8_t s_protocols;

static uint8_t s_mac[6];
static char s_device_id[18];
static char s_device_hex[13];
static uint8_t s_pk[32];
static char s_pk_hex[65];
static char s_pi[37], s_psi[37];
static char s_fex[16];

/* Identity */

const char *ap2_device_id(void) {
    return s_device_id;
}

const uint8_t *ap2_public_key(void) {
    return s_pk;
}

const char *ap2_public_key_hex(void) {
    return s_pk_hex;
}

const char *ap2_pi(void) {
    return s_pi;
}

const char *ap2_psi(void) {
    return s_psi;
}

const char *ap2_fex(void) {
    return s_fex;
}

uint64_t ap2_features(void) {
    return FEATURES;
}

void ap2_name(char *out, size_t len) {
    portENTER_CRITICAL(&s_lock);
    snprintf(out, len, "%s", s_name);
    portEXIT_CRITICAL(&s_lock);
}

// Stable per device: a name-based UUID over the MAC.
static void derived_uuid(char out[37], const uint8_t mac[6], const char *label) {
    uint8_t h[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const uint8_t *)label, strlen(label));
    mbedtls_sha256_update(&ctx, mac, 6);
    mbedtls_sha256_finish(&ctx, h);
    mbedtls_sha256_free(&ctx);
    h[6] = (h[6] & 0x0F) | 0x50;
    h[8] = (h[8] & 0x3F) | 0x80;
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", h[0], h[1], h[2], h[3],
             h[4], h[5], h[6], h[7], h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15]);
}

static void make_identity(const uint8_t mac[6]) {
    memcpy(s_mac, mac, sizeof(s_mac));
    snprintf(s_device_id, sizeof(s_device_id), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    snprintf(s_device_hex, sizeof(s_device_hex), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    // pair_ap derives the long-term Ed25519 key from the device id.
    pair_public_key_get(PAIR_SERVER_HOMEKIT, s_pk, s_device_id);
    for (int i = 0; i < 32; i++) {
        snprintf(s_pk_hex + i * 2, 3, "%02x", s_pk[i]);
    }
    // The labels predate the rename; changing them would change the identity senders know.
    derived_uuid(s_pi, mac, "ap2ng-pi");
    derived_uuid(s_psi, mac, "ap2ng-psi");

    uint8_t fex[8];
    uint64_t f = FEATURES;
    for (int i = 0; i < 8; i++, f >>= 8) {
        fex[i] = (uint8_t)f;
    }
    size_t olen = 0;
    mbedtls_base64_encode((uint8_t *)s_fex, sizeof(s_fex), &olen, fex, sizeof(fex));
    char *pad = strchr(s_fex, '=');
    if (pad) {
        *pad = '\0';
    }
}

/* Events */

const uint8_t *ap2_mac(void) {
    return s_mac;
}

void ap2_emit(const raop_event_t *event) {
    s_ctx.event_cb(event);
}

bool ap2_event_cb(const raop_event_t *event) {
    return s_ctx.event_cb(event);
}

void ap2_emit_type(raop_event_type_t type) {
    raop_event_t event = {.type = type};
    s_ctx.event_cb(&event);
}

void ap2_push_pcm(const int16_t *pcm, size_t frames, int64_t playtime_us) {
    s_ctx.pcm_cb(pcm, frames, playtime_us);
}

float ap2_volume_db(void) {
    portENTER_CRITICAL(&s_lock);
    float db = s_volume_db;
    portEXIT_CRITICAL(&s_lock);
    return db;
}

static float db_to_linear(float db) {
    if (db <= AP2_VOLUME_MIN_DB) {
        return 0.0f;
    }
    return db >= 0.0f ? 1.0f : (db - AP2_VOLUME_MIN_DB) / -AP2_VOLUME_MIN_DB;
}

void ap2_set_volume_db(float db) {
    portENTER_CRITICAL(&s_lock);
    s_volume_db = db;
    portEXIT_CRITICAL(&s_lock);
    raop_event_t ev = {.type = RAOP_EVENT_VOLUME, .volume = db_to_linear(db)};
    ap2_emit(&ev);
}

/* Sockets */

int ap2_recv(int fd, void *buf, size_t len, volatile bool *stop) {
    for (;;) {
        int n = recv(fd, buf, len, 0);
        if (n >= 0 || !stop || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            return n;
        }
        if (*stop) {
            return -1;
        }
    }
}

int ap2_listen_any(int type, uint16_t *port) {
    int fd = socket(AF_INET, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_ANY)};
    socklen_t len = sizeof(addr);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || (type == SOCK_STREAM && listen(fd, 1) < 0) ||
        getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(addr.sin_port);
    return fd;
}

/* Memory */

void *ap2_malloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void ap2_free(void *p) {
    heap_caps_free(p);
}

void ap2_log_stack(void) {
    ESP_LOGI(TAG, "task %s: %u bytes of stack never used", pcTaskGetName(NULL),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

void ap2_log_heap(void) {
    ESP_LOGI(TAG, "heap: internal %u free (low %u, largest %u), PSRAM %u free (low %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

bool ap2_task_start(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                      BaseType_t core, bool psram_stack) {
    UBaseType_t caps = (psram_stack ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL) | MALLOC_CAP_8BIT;
    return xTaskCreatePinnedToCoreWithCaps(fn, name, stack, arg, prio, NULL, core, caps) == pdPASS;
}

void ap2_task_exit(void) {
    ap2_log_stack();
    vTaskDeleteWithCaps(NULL);
    for (;;) {
    }
}

/* mDNS */

typedef struct {
    char key[12];
    char value[72];
} txt_t;

static int add_txt(txt_t *t, int n, const char *key, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int add_txt(txt_t *t, int n, const char *key, const char *fmt, ...) {
    snprintf(t[n].key, sizeof(t[n].key), "%s", key);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t[n].value, sizeof(t[n].value), fmt, ap);
    va_end(ap);
    return n + 1;
}

static void to_mdns(const txt_t *t, int n, mdns_txt_item_t *items) {
    for (int i = 0; i < n; i++) {
        items[i] = (mdns_txt_item_t){.key = t[i].key, .value = t[i].value};
    }
}

// As shairport-sync's secondary (_airplay) and primary (_raop) records,
// the latter with what AirPlay 1 senders look for too.
static esp_err_t advertise_airplay(const char *name) {
    unsigned long f_lo = (unsigned long)(FEATURES & 0xFFFFFFFF), f_hi = (unsigned long)(FEATURES >> 32);
    static txt_t airplay[16];
    mdns_txt_item_t items[16];

    int n = 0;
    n = add_txt(airplay, n, "acl", "0");
    n = add_txt(airplay, n, "deviceid", "%s", s_device_id);
    n = add_txt(airplay, n, "fex", "%s", s_fex);
    n = add_txt(airplay, n, "features", "0x%lX,0x%lX", f_lo, f_hi);
    n = add_txt(airplay, n, "flags", "0x%X", AP2_STATUS_FLAGS);
    n = add_txt(airplay, n, "gid", "%s", s_pi);
    n = add_txt(airplay, n, "igl", "0");
    n = add_txt(airplay, n, "gcgl", "0");
    n = add_txt(airplay, n, "model", "%s", AP2_MODEL);
    n = add_txt(airplay, n, "protovers", "1.1");
    n = add_txt(airplay, n, "pi", "%s", s_pi);
    n = add_txt(airplay, n, "psi", "%s", s_psi);
    n = add_txt(airplay, n, "pk", "%s", s_pk_hex);
    n = add_txt(airplay, n, "srcvers", "%s", AP2_SRCVERS);
    n = add_txt(airplay, n, "fv", "%s", AP2_FIRMWARE);
    to_mdns(airplay, n, items);
    return mdns_service_add(name, "_airplay", "_tcp", AP2_RTSP_PORT, items, (size_t)n);
}

static esp_err_t advertise_raop(const char *name) {
    unsigned long f_lo = (unsigned long)(FEATURES & 0xFFFFFFFF), f_hi = (unsigned long)(FEATURES >> 32);
    static txt_t raop[20];
    mdns_txt_item_t items[20];

    int m = 0;
    m = add_txt(raop, m, "txtvers", "1");
    m = add_txt(raop, m, "ch", "2");
    m = add_txt(raop, m, "ss", "16");
    m = add_txt(raop, m, "sr", "%d", AP2_RATE);
    m = add_txt(raop, m, "ek", "1");
    m = add_txt(raop, m, "cn", "0,1");
    m = add_txt(raop, m, "da", "true");
    m = add_txt(raop, m, "et", "0,1");
    m = add_txt(raop, m, "ft", "0x%lX,0x%lX", f_lo, f_hi);
    m = add_txt(raop, m, "fv", "%s", AP2_FIRMWARE);
    m = add_txt(raop, m, "sf", "0x%X", AP2_STATUS_FLAGS);
    m = add_txt(raop, m, "md", "0,1,2");
    m = add_txt(raop, m, "am", "%s", AP2_MODEL);
    m = add_txt(raop, m, "pk", "%s", s_pk_hex);
    m = add_txt(raop, m, "tp", "UDP");
    m = add_txt(raop, m, "vn", "65537");
    m = add_txt(raop, m, "vs", "%s", AP2_SRCVERS);
    to_mdns(raop, m, items);
    char instance[NAME_MAX_LEN + 16];
    snprintf(instance, sizeof(instance), "%s@%s", s_device_hex, name);
    return mdns_service_add(instance, "_raop", "_tcp", AP2_RTSP_PORT, items, (size_t)m);
}

// AirPlay 2 senders find the receiver through _airplay, AirPlay 1 senders
// through _raop. Only the protocols in the mask are touched: every
// mdns_service_add() restarts probing and drops what is still queued from
// the last one, so records that stay should be left alone.
static void advertise(const char *name, uint8_t protocols) {
    esp_err_t err = ESP_OK;
    if (protocols & RAOP_PROTOCOL_AIRPLAY2) {
        err = advertise_airplay(name);
    }
    if (err == ESP_OK && (protocols & RAOP_PROTOCOL_AIRPLAY1)) {
        err = advertise_raop(name);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS advertisement failed: %s", esp_err_to_name(err));
    }
}

static void unadvertise(uint8_t protocols) {
    if (protocols & RAOP_PROTOCOL_AIRPLAY2) {
        mdns_service_remove("_airplay", "_tcp");
    }
    if (protocols & RAOP_PROTOCOL_AIRPLAY1) {
        mdns_service_remove("_raop", "_tcp");
    }
}

bool ap2_protocol_enabled(uint8_t protocol) {
    portENTER_CRITICAL(&s_lock);
    bool enabled = s_protocols & protocol;
    portEXIT_CRITICAL(&s_lock);
    return enabled;
}

/* raop.h */

raop_ctx_t *raop_create(const char *name, const uint8_t mac[6], uint8_t protocols, raop_event_cb_t event_cb,
                        raop_pcm_cb_t pcm_cb) {
    s_ctx.event_cb = event_cb;
    s_ctx.pcm_cb = pcm_cb;
    portENTER_CRITICAL(&s_lock);
    snprintf(s_name, sizeof(s_name), "%s", name);
    s_protocols = protocols;
    portEXIT_CRITICAL(&s_lock);

    int64_t t0 = esp_timer_get_time();
    make_identity(mac);
    dacp_init();
    if (!raop_setup_init()) {
        return NULL;
    }
    session_init();
    if (ptp_init() != ESP_OK || rtsp_start() != ESP_OK) {
        ESP_LOGE(TAG, "AirPlay 2 receiver failed to start");
        return NULL;
    }
    ap2_log_heap();
    advertise(name, protocols);
    ESP_LOGI(TAG, "AirPlay 2 receiver \"%s\" (%s) up in %lld ms", name, s_device_id,
             (long long)((esp_timer_get_time() - t0) / 1000));
    return &s_ctx;
}

void raop_set_name(raop_ctx_t *ctx, const char *name) {
    (void)ctx;
    portENTER_CRITICAL(&s_lock);
    snprintf(s_name, sizeof(s_name), "%s", name);
    portEXIT_CRITICAL(&s_lock);
    char instance[NAME_MAX_LEN + 16];
    snprintf(instance, sizeof(instance), "%s@%s", s_device_hex, name);
    mdns_service_instance_name_set("_airplay", "_tcp", name);
    mdns_service_instance_name_set("_raop", "_tcp", instance);
}

void raop_set_protocols(raop_ctx_t *ctx, uint8_t protocols) {
    (void)ctx;
    char name[NAME_MAX_LEN + 1];
    portENTER_CRITICAL(&s_lock);
    uint8_t dropped = s_protocols & ~protocols;
    uint8_t added = protocols & ~s_protocols;
    s_protocols = protocols;
    snprintf(name, sizeof(name), "%s", s_name);
    portEXIT_CRITICAL(&s_lock);
    if (!dropped && !added) {
        return;
    }

    unadvertise(dropped);
    advertise(name, added);

    // Idle connections go too; a sender just reconnects to what's left.
    if (dropped & (session_is_airplay1() ? RAOP_PROTOCOL_AIRPLAY1 : RAOP_PROTOCOL_AIRPLAY2)) {
        rtsp_disconnect_all();
    }
    ESP_LOGI(TAG, "AirPlay 1 %s, AirPlay 2 %s", protocols & RAOP_PROTOCOL_AIRPLAY1 ? "on" : "off",
             protocols & RAOP_PROTOCOL_AIRPLAY2 ? "on" : "off");
}

void raop_abort(raop_ctx_t *ctx) {
    (void)ctx;
    rtsp_disconnect_all();
}

static const struct {
    const char *dacp;
    mr_command_t mr;
} s_commands[] = {
    {"play", MR_PLAY},
    {"pause", MR_PAUSE},
    {"playpause", MR_TOGGLE_PLAY_PAUSE},
    {"stop", MR_STOP},
    {"nextitem", MR_NEXT_TRACK},
    {"previtem", MR_PREVIOUS_TRACK},
};

bool raop_remote_command(raop_ctx_t *ctx, const char *command) {
    (void)ctx;
    if (session_is_airplay1()) {
        return dacp_command(command);
    }
    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        if (!strcmp(command, s_commands[i].dacp)) {
            return session_media_command(s_commands[i].mr);
        }
    }
    return false;
}

// AirPlay 1 senders take it over DACP. AirPlay 2 has no way to move the
// sender's slider (MediaRemote carries no volume), so only what /info and
// GET_PARAMETER report follows the knob.
bool raop_remote_volume(raop_ctx_t *ctx, float volume) {
    (void)ctx;
    if (session_is_airplay1()) {
        return dacp_volume(volume);
    }
    float db = volume > 0.0f ? (1.0f - volume) * AP2_VOLUME_MIN_DB : AP2_VOLUME_MUTE_DB;
    portENTER_CRITICAL(&s_lock);
    s_volume_db = db;
    portEXIT_CRITICAL(&s_lock);
    return false;
}

uint32_t raop_time_ms(void) {
    return (uint32_t)(esp_timer_get_time() / 1000);
}
