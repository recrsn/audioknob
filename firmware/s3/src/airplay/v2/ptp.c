/*
 * PTP follower with nqptp's behaviour (Mike Brady's companion to
 * shairport-sync), reimplemented for one sender.
 *
 * Each Follow_Up gives (master time at departure) - (local time at
 * arrival): the clock offset less that packet's network delay. Delay only
 * ever makes the sample smaller, so rises are trusted and falls are not:
 * for the first second of a timing sequence rises are taken whole and
 * falls ignored, after that a rise moves the offset by 1/16 and a fall,
 * capped at 2.5 ms, by 1/256. The offset is only published while play is
 * active; a clock that may have slept since play last stopped starts a new
 * sequence.
 *
 * Apple senders can go quiet; an Announce from an apparently better clock
 * followed by one from a worse one makes them renegotiate and resume. That
 * is sent on the first Announce seen, when three Announces pass with no
 * Follow_Up, and when the grandmaster's time stops early in a sequence.
 */

#include "ptp.h"

#include "ap2.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define EVENT_PORT 319
#define GENERAL_PORT 320

#define HDR_LEN 34
#define TIMESTAMP_LEN 10
#define ANNOUNCE_LEN 64

#define MSG_FOLLOW_UP 0x8
#define MSG_ANNOUNCE 0xB

#define MS 1000000LL
#define SEC 1000000000LL

// Smoothing.
#define SETTLING_NS SEC
#define FALL_CAP_NS (-2500 * 1000LL)
#define RISE_DIV 16
#define FALL_DIV 256

// After "T" the clock is assumed awake this long, after "E" this long.
#define VALID_AFTER_PEER_NS (5 * SEC)
#define VALID_AFTER_END_NS (2250 * MS)

// A minute of Follow_Ups at 8 Hz with the same origin time: the clock stopped.
#define STOPPED_FOLLOW_UPS (8 * 60)
// Only a clock stopped this early in its sequence is worth waking.
#define WAKE_STOPPED_WITHIN_NS (62 * SEC)
#define SILENT_ANNOUNCES 3
#define WAKE_GAP_NS (150 * MS)
#define HOUSEKEEPING_NS (50 * MS)

#define TASK_PRIO 12
#define TASK_CORE 0
#define TASK_STACK 4096
#define STATS_EVERY_FOLLOW_UPS 240

// One followed clock; wiped by every new timing peer.
typedef struct {
    uint32_t ip;  // network order, 0 = no timing peer
    uint64_t clock_id;  // sender's, from its Announces; 0 until one arrives
    uint64_t grandmaster;
    uint8_t priority1, priority2;
    bool wakeup_sent;
    int announces_without_follow_up;
    int follow_ups;  // counts to 100

    int64_t prev_origin;
    int identical_origins;
    uint64_t prev_grandmaster;  // whose time prev_offset is in
    int64_t prev_offset;        // smoothed
    int64_t prev_offset_t;      // 0: the next sample starts a sequence
    int64_t prev_follow_up_t;
    int64_t mastership_start;
    int64_t change_offset;
} clock_rec_t;

static struct {
    SemaphoreHandle_t lock;
    clock_rec_t clk;
    bool active;
    bool reset_smoothing;
    int64_t valid_until;  // while inactive; 0 = unknown

    // The second, worse Announce of a wake sequence.
    int64_t wake_at;  // 0 = none due
    uint32_t wake_ip;
    uint8_t wake_priority1, wake_priority2;

    int general_fd;
    uint64_t self_id;
    uint32_t stats_follow_ups;
} s;

static const char *TAG = "ptp";

static portMUX_TYPE s_pub_lock = portMUX_INITIALIZER_UNLOCKED;
static ptp_clock_t s_pub;  // clock_id 0 = no master

int64_t ptp_local_ns(void) {
    return esp_timer_get_time() * 1000;
}

static uint64_t be(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void put_be(uint8_t *p, uint64_t v, size_t n) {
    for (size_t i = n; i > 0; i--) {
        p[i - 1] = (uint8_t)v;
        v >>= 8;
    }
}

static int64_t timestamp_ns(const uint8_t *p) {
    return (int64_t)be(p, 6) * SEC + (int64_t)be(p + 6, 4);
}

// correctionField is ns scaled by 2^16.
static int64_t correction_ns(const uint8_t *hdr) {
    return (int64_t)be(hdr + 8, 8) / 65536;
}

static void publish(const ptp_clock_t *c) {
    portENTER_CRITICAL(&s_pub_lock);
    s_pub = *c;
    portEXIT_CRITICAL(&s_pub_lock);
}

static void publish_none(void) {
    publish(&(ptp_clock_t){0});
}

/* Waking the sender's clock */

static void send_announce(uint32_t ip, uint8_t priority1, uint8_t priority2) {
    uint8_t m[ANNOUNCE_LEN] = {0};
    m[0] = 0x10 | MSG_ANNOUNCE;  // transportSpecific 1: 802.1AS
    m[1] = 0x02;
    put_be(m + 2, ANNOUNCE_LEN, 2);
    put_be(m + 6, 0x0408, 2);  // flags: PTP timescale, two-step
    put_be(m + 20, s.self_id, 8);
    put_be(m + 28, 0x8008, 2);  // port number
    m[32] = 0x05;
    m[33] = 0xFE;  // log interval: 1/4 s
    put_be(m + 44, 37, 2);  // UTC offset
    m[47] = priority1;
    put_be(m + 48, 0xF8FE436A, 4);  // clock class 248, accuracy unknown
    m[52] = priority2;
    put_be(m + 53, s.self_id, 8);
    m[63] = 160;  // internal oscillator

    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(GENERAL_PORT),
        .sin_addr.s_addr = ip,
    };
    if (sendto(s.general_fd, m, sizeof(m), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        ESP_LOGW(TAG, "can't send Announce: errno %d", errno);
    }
}

// Better than the sender now, worse a moment later, so it renegotiates
// and stays master.
static void wake(const clock_rec_t *c, int64_t now) {
    ESP_LOGI(TAG, "waking clock %016llx", (unsigned long long)c->clock_id);
    if (c->priority1 > 2) {
        send_announce(c->ip, c->priority1 - 1, c->priority2);
    } else {
        send_announce(c->ip, 248, 248);
    }
    s.wake_ip = c->ip;
    s.wake_priority1 = c->priority1 < 254 ? c->priority1 + 1 : 250;
    s.wake_priority2 = c->priority2;
    s.wake_at = now + WAKE_GAP_NS;
}

static void housekeeping(int64_t now) {
    if (s.wake_at && now >= s.wake_at) {
        s.wake_at = 0;
        send_announce(s.wake_ip, s.wake_priority1, s.wake_priority2);
    }
    clock_rec_t *c = &s.clk;
    if (c->announces_without_follow_up == SILENT_ANNOUNCES && c->follow_ups == 0) {
        c->announces_without_follow_up++;  // only once
        ESP_LOGI(TAG, "clock %016llx is silent", (unsigned long long)c->clock_id);
        wake(c, now);
    }
}

/* Messages */

static void on_announce(clock_rec_t *c, const uint8_t *buf, size_t len, int64_t rx) {
    if (len < ANNOUNCE_LEN) {
        return;
    }
    c->clock_id = be(buf + 20, 8);
    c->priority1 = buf[47];
    c->priority2 = buf[52];
    c->grandmaster = be(buf + 53, 8);
    if (c->announces_without_follow_up < 5) {
        c->announces_without_follow_up++;
    }
    if (!c->wakeup_sent) {
        c->wakeup_sent = true;
        wake(c, rx);
    }
}

static void check_stopped(clock_rec_t *c, int64_t origin, int64_t rx) {
    if (origin != c->prev_origin || c->clock_id != c->grandmaster) {
        c->identical_origins = 0;
        return;
    }
    if (++c->identical_origins == STOPPED_FOLLOW_UPS) {
        int64_t held = c->mastership_start ? rx - c->mastership_start : 0;
        ESP_LOGW(TAG, "clock %016llx stopped after %lld s as master", (unsigned long long)c->clock_id,
                 (long long)(held / SEC));
        if (held <= WAKE_STOPPED_WITHIN_NS) {
            wake(c, rx);
        }
    }
}

// Rises are shorter delays and trusted; falls are longer delays, or drift.
static int64_t smooth(const clock_rec_t *c, int64_t offset, int64_t rx) {
    int64_t held = c->mastership_start ? rx - c->mastership_start : 0;
    int64_t jitter = offset - c->prev_offset;
    if (jitter < 0) {
        if (held <= SETTLING_NS) {
            return c->prev_offset;
        }
        return c->prev_offset + (jitter < FALL_CAP_NS ? FALL_CAP_NS : jitter) / FALL_DIV;
    }
    return c->prev_offset + (held < SETTLING_NS ? jitter : jitter / RISE_DIV);
}

// A new grandmaster's time can differ from the old one's; the step goes to
// the player as change_offset while smoothing carries on from the new time.
static void check_grandmaster(clock_rec_t *c, int64_t offset) {
    if (c->prev_grandmaster == c->grandmaster) {
        return;
    }
    c->prev_offset_t = 0;
    if (c->prev_grandmaster) {
        c->change_offset = c->prev_offset - offset;
        ESP_LOGI(TAG, "grandmaster %016llx -> %016llx, %+.3f ms", (unsigned long long)c->prev_grandmaster,
                 (unsigned long long)c->grandmaster, (double)c->change_offset / MS);
        c->prev_offset_t = c->prev_follow_up_t;
        c->prev_offset = offset;
    }
}

static void log_stats(const clock_rec_t *c, int64_t offset, int64_t smoothed, int64_t rx) {
    if (++s.stats_follow_ups % STATS_EVERY_FOLLOW_UPS) {
        return;
    }
    ESP_LOGI(TAG, "clock %016llx%s: %lld s as master, last sample %+.3f ms off", (unsigned long long)c->grandmaster,
             s.active ? "" : " (inactive)", (long long)((rx - c->mastership_start) / SEC),
             (double)(offset - smoothed) / MS);
    ap2_log_heap();
    if (s.stats_follow_ups % (STATS_EVERY_FOLLOW_UPS * 10) == 0) {
        ap2_log_stack();
    }
}

static void on_follow_up(clock_rec_t *c, const uint8_t *buf, size_t len, int64_t rx) {
    if (c->clock_id && len >= HDR_LEN + TIMESTAMP_LEN) {
        int64_t origin = timestamp_ns(buf + HDR_LEN) + correction_ns(buf);
        if (c->follow_ups < 100) {
            c->follow_ups++;
        }
        c->announces_without_follow_up = 0;
        check_stopped(c, origin, rx);
        c->prev_origin = origin;

        int64_t offset = origin - rx;
        check_grandmaster(c, offset);

        if (s.reset_smoothing) {
            // This sample may predate the clock waking; the next one starts afresh.
            s.reset_smoothing = false;
            c->mastership_start = 0;
            c->prev_offset = 0;
            c->prev_offset_t = 0;
        } else {
            int64_t smoothed = offset;
            if (c->prev_offset_t) {
                smoothed = smooth(c, offset, rx);
            } else {
                c->mastership_start = rx;
                ESP_LOGI(TAG, "timing sequence starts on clock %016llx", (unsigned long long)c->grandmaster);
            }
            if (s.active) {
                publish(&(ptp_clock_t){
                    .clock_id = c->grandmaster,
                    .local_ns = rx,
                    .offset_ns = smoothed,
                    .clock_change_ns = c->change_offset,
                    .mastership_start_ns = c->mastership_start,
                });
            } else {
                publish_none();
            }
            c->prev_offset = smoothed;
            c->prev_offset_t = rx;
            log_stats(c, offset, smoothed, rx);
        }
        c->prev_grandmaster = c->grandmaster;
    }
    c->prev_follow_up_t = rx;
}

static void handle(const uint8_t *buf, size_t len, int64_t rx) {
    clock_rec_t *c = &s.clk;
    switch (buf[0] & 0x0F) {
    case MSG_ANNOUNCE:
        on_announce(c, buf, len, rx);
        break;
    case MSG_FOLLOW_UP:
        on_follow_up(c, buf, len, rx);
        break;
    }
}

/* Task */

static int open_port(uint16_t port) {
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void receive(int fd, uint16_t port, uint8_t *buf, size_t size) {
    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = recvfrom(fd, buf, size, 0, (struct sockaddr *)&from, &from_len);
    int64_t rx = ptp_local_ns();
    // PTP replies come from the port they go to.
    if (n < HDR_LEN || ntohs(from.sin_port) != port) {
        return;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (s.clk.ip && from.sin_addr.s_addr == s.clk.ip) {
        handle(buf, (size_t)n, rx);
    }
    xSemaphoreGive(s.lock);
}

static void ptp_task(void *arg) {
    const uint16_t ports[2] = {EVENT_PORT, GENERAL_PORT};
    int fds[2] = {open_port(EVENT_PORT), open_port(GENERAL_PORT)};
    if (fds[0] < 0 || fds[1] < 0) {
        ESP_LOGE(TAG, "can't bind PTP ports");
        ap2_task_exit();
    }
    s.general_fd = fds[1];
    uint8_t buf[128];
    int64_t next_housekeeping = ptp_local_ns();

    for (;;) {
        int64_t now = ptp_local_ns();
        xSemaphoreTake(s.lock, portMAX_DELAY);
        if (now >= next_housekeeping) {
            housekeeping(now);
            next_housekeeping = now + HOUSEKEEPING_NS;
        }
        int64_t due = next_housekeeping;
        if (s.wake_at && s.wake_at < due) {
            due = s.wake_at;
        }
        xSemaphoreGive(s.lock);

        int64_t wait_us = (due - now) / 1000;
        struct timeval tv = {.tv_sec = 0, .tv_usec = wait_us > 0 ? wait_us : 0};
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fds[0], &set);
        FD_SET(fds[1], &set);
        if (select((fds[0] > fds[1] ? fds[0] : fds[1]) + 1, &set, NULL, NULL, &tv) <= 0) {
            continue;
        }
        for (int i = 0; i < 2; i++) {
            if (FD_ISSET(fds[i], &set)) {
                receive(fds[i], ports[i], buf, sizeof(buf));
            }
        }
    }
}

// EUI-64 from the station MAC, as IEEE 1588 7.5.2.2.2 builds it.
static uint64_t self_clock_id(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint8_t id[8] = {mac[0], mac[1], mac[2], 0xFF, 0xFE, mac[3], mac[4], mac[5]};
    return be(id, 8);
}

esp_err_t ptp_init(void) {
    s.lock = xSemaphoreCreateMutex();
    if (!s.lock) {
        return ESP_ERR_NO_MEM;
    }
    s.self_id = self_clock_id();
    return ap2_task_start(ptp_task, "ptp", TASK_STACK, NULL, TASK_PRIO, TASK_CORE, false) ? ESP_OK : ESP_ERR_NO_MEM;
}

/* Control */

void ptp_set_timing_peer(const char *ip) {
    struct in_addr addr = {0};
    if (ip && !inet_aton(ip, &addr)) {
        addr.s_addr = 0;
    }
    int64_t now = ptp_local_ns();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    memset(&s.clk, 0, sizeof(s.clk));
    s.wake_at = 0;
    if (addr.s_addr) {
        s.clk.ip = addr.s_addr;
        s.valid_until = now + VALID_AFTER_PEER_NS;
    } else {
        s.active = false;
        publish_none();
    }
    xSemaphoreGive(s.lock);
}

void ptp_play_begin(void) {
    int64_t now = ptp_local_ns();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    // Inactive past the time it was known to be awake, the clock may have
    // slept and come back with a lower offset that smoothing would take
    // for delay.
    if (!s.active && (!s.valid_until || now > s.valid_until)) {
        ESP_LOGI(TAG, "clock may have slept, resetting smoothing");
        s.reset_smoothing = true;
    }
    s.active = true;
    s.valid_until = 0;
    xSemaphoreGive(s.lock);
}

void ptp_play_end(void) {
    int64_t now = ptp_local_ns();
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (s.active) {
        s.active = false;
        s.valid_until = now + VALID_AFTER_END_NS;
    }
    xSemaphoreGive(s.lock);
}

bool ptp_clock(ptp_clock_t *out) {
    portENTER_CRITICAL(&s_pub_lock);
    *out = s_pub;
    portEXIT_CRITICAL(&s_pub_lock);
    return out->clock_id != 0;
}
