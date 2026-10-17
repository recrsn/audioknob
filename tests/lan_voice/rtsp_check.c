#include "runtime.h"
#include "talk_rtsp.h"
#include "http_util.h"
#include <stdatomic.h>
static atomic_bool active;
static atomic_int received;
uint32_t esp_random(void) {
    static atomic_uint value = 12;
    return atomic_fetch_add(&value, 1);
}
void device_name_get(char *out, size_t n) {
    snprintf(out, n, "Host test knob");
}
bool wifi_get_station(char *ssid, size_t slen, char *ip, size_t ilen) {
    snprintf(ssid, slen, "Test");
    snprintf(ip, ilen, "127.0.0.1");
    return true;
}
static bool start(uint32_t ip, uint16_t port, uint16_t seq, const char *name) {
    assert(ip == inet_addr("127.0.0.1") && port == 42425 && seq == 65000 && !strcmp(name, "Host test knob"));
    atomic_store(&active, true);
    return true;
}
static void stop(void) {
    atomic_store(&active, false);
}
static bool is_active(void) {
    return atomic_load(&active);
}
static bool message(uint32_t ip, const char *name, const char *path, const char *type, const void *data, size_t size) {
    assert(ip == inet_addr("127.0.0.1") && !strcmp(name, "Host test knob"));
    if (!strcmp(path, "/messages"))
        assert(!strcmp(type, "application/json") && size == 7 && !memcmp(data, "{\"a\":1}", 7));
    else {
        const uint8_t expected[] = {1, 0, 2, 0, 3};
        assert(!strcmp(path, "/voice") && !strcmp(type, "audio/ogg") && size == 5 && !memcmp(data, expected, 5));
    }
    atomic_fetch_add(&received, 1);
    return true;
}
static void legacy_parser(void) {
    int fd[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, fd) == 0);
    const char *request = "ANNOUNCE rtsp://127.0.0.1/test RTSP/1.0\r\nCSeq: 1\r\nContent-Length: 3\r\n\r\nabc";
    assert(send(fd[0], request, strlen(request), 0) == (int)strlen(request));
    char method[32], *body = NULL;
    int n;
    key_data_t headers[8] = {0};
    assert(http_parse(fd[1], method, sizeof(method), headers, 7, &body, &n));
    assert(!strcmp(method, "ANNOUNCE") && n == 3 && !strcmp(body, "abc") && !strcmp(kd_lookup(headers, "cseq"), "1"));
    free(body);
    kd_free(headers);
    close(fd[0]);
    close(fd[1]);
}
int main(void) {
    legacy_parser();
    talk_rtsp_init(&(talk_rtsp_ops_t){start, stop, is_active, message});
    usleep(200000);
    uint16_t port = 0;
    int fd = talk_rtsp_begin(inet_addr("127.0.0.1"), 65000, 123456, &port);
    assert(fd >= 0 && port == 42425 && talk_rtsp_alive(fd) && is_active());
    int busy = talk_rtsp_begin(inet_addr("127.0.0.1"), 65000, 123456, &port);
    assert(busy < 0 && talk_rtsp_alive(fd) && is_active());
    usleep(50000);
    assert(talk_rtsp_message(inet_addr("127.0.0.1"), "/messages", "{\"a\":1}"));
    const uint8_t binary[] = {1, 0, 2, 0, 3};
    assert(talk_rtsp_message_bytes(inet_addr("127.0.0.1"), "/voice", "audio/ogg", binary, sizeof(binary)));
    assert(atomic_load(&received) == 2 && is_active());
    talk_rtsp_end(fd);
    for (int i = 0; i < 100 && is_active(); i++)
        usleep(10000);
    assert(!is_active());
    puts("Shared AirPlay parser, RTSP call setup/close, and concurrent JSON/binary messages passed");
}
