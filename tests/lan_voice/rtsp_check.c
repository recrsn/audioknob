#include "runtime.h"
#include "talk/rtsp.h"
#include "airplay/shared/http_util.h"
#include <stdatomic.h>

// The callee's side, as talk.c would answer it.
static atomic_uint ringing_call, ended_call, next_call = 1;
static atomic_int verdict = TALK_ANSWER_PENDING;
static atomic_bool active;
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
static uint32_t ring(uint32_t ip, uint16_t port, uint16_t seq, const char *name) {
    assert(ip == inet_addr("127.0.0.1") && port == 42425 && seq == 65000 && !strcmp(name, "Host test knob"));
    if (atomic_load(&ringing_call))
        return 0;
    uint32_t call = atomic_fetch_add(&next_call, 1);
    atomic_store(&verdict, TALK_ANSWER_PENDING);
    atomic_store(&ringing_call, call);
    return call;
}
static talk_answer_t answer(uint32_t call, uint16_t *seq, uint32_t *time) {
    assert(call == atomic_load(&ringing_call));
    *seq = 4242;
    *time = 99;
    talk_answer_t a = atomic_load(&verdict);
    if (a == TALK_ANSWER_ACCEPTED)
        atomic_store(&active, true);
    return a;
}
static void stop(uint32_t call) {
    atomic_store(&ended_call, call);
    atomic_store(&ringing_call, 0);
    atomic_store(&active, false);
}
static bool is_active(uint32_t call) {
    return call == atomic_load(&ringing_call) && atomic_load(&active);
}
static void wait_for(atomic_uint *value, unsigned want) {
    for (int i = 0; i < 200 && atomic_load(value) != want; i++)
        usleep(10000);
    assert(atomic_load(value) == want);
}
static talk_dial_t wait_answer(int fd, uint16_t *seq) {
    talk_dial_t r = TALK_DIAL_RINGING;
    for (int i = 0; i < 200 && r == TALK_DIAL_RINGING; i++) {
        r = talk_rtsp_answer(fd, seq);
        usleep(10000);
    }
    return r;
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
    talk_rtsp_init(&(talk_rtsp_ops_t){ring, answer, stop, is_active});
    usleep(200000);
    uint16_t port = 0, seq = 0;

    // Rings until answered; the answer carries the callee's stream start.
    int fd = talk_rtsp_dial(inet_addr("127.0.0.1"), 65000, 123456, &port);
    assert(fd >= 0 && port == 42425);
    wait_for(&ringing_call, 1);
    usleep(150000);
    assert(talk_rtsp_answer(fd, &seq) == TALK_DIAL_RINGING);
    atomic_store(&verdict, TALK_ANSWER_ACCEPTED);
    assert(wait_answer(fd, &seq) == TALK_DIAL_ANSWERED && seq == 4242 && talk_rtsp_alive(fd));

    // One call at a time.
    int busy = talk_rtsp_dial(inet_addr("127.0.0.1"), 65000, 123456, &port);
    assert(busy >= 0 && wait_answer(busy, &seq) == TALK_DIAL_BUSY);
    talk_rtsp_end(busy);

    // Hanging up on either side ends it for the other.
    talk_rtsp_end(fd);
    wait_for(&ended_call, 1);
    assert(!atomic_load(&active));

    // Declined.
    fd = talk_rtsp_dial(inet_addr("127.0.0.1"), 65000, 123456, &port);
    assert(fd >= 0);
    wait_for(&ringing_call, 2);
    atomic_store(&verdict, TALK_ANSWER_DECLINED);
    assert(wait_answer(fd, &seq) == TALK_DIAL_DECLINED);
    wait_for(&ended_call, 2);
    talk_rtsp_end(fd);

    // The caller giving up while it rings stops the ring.
    fd = talk_rtsp_dial(inet_addr("127.0.0.1"), 65000, 123456, &port);
    assert(fd >= 0);
    wait_for(&ringing_call, 3);
    talk_rtsp_end(fd);
    wait_for(&ended_call, 3);

    // The callee hanging up closes the caller's connection.
    fd = talk_rtsp_dial(inet_addr("127.0.0.1"), 65000, 123456, &port);
    wait_for(&ringing_call, 4);
    atomic_store(&verdict, TALK_ANSWER_ACCEPTED);
    assert(wait_answer(fd, &seq) == TALK_DIAL_ANSWERED);
    atomic_store(&active, false);
    for (int i = 0; i < 100 && talk_rtsp_alive(fd); i++)
        usleep(10000);
    assert(!talk_rtsp_alive(fd));
    talk_rtsp_end(fd);
    puts("Shared AirPlay parser and RTSP call ring/answer/busy/decline/hang-up passed");
}
