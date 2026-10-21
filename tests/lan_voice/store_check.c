#include "runtime.h"
#include "talk/flash_message.h"
#include "talk/message_store.h"
#include "talk/voice/notes.h"
#include "talk/voice/ogg.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include <stdatomic.h>
#include <sys/stat.h>
static atomic_int sent, acks;
uint32_t esp_random(void) {
    static atomic_uint value = 100;
    return atomic_fetch_add(&value, 1) ^ ((uint32_t)getpid() << 16);
}
void device_name_get(char *out, size_t n) {
    snprintf(out, n, "Host inbox");
}
bool talk_rtsp_message(uint32_t ip, const char *path, const char *json) {
    assert(ip == inet_addr("127.0.0.2"));
    assert(json && strstr(json, "id"));
    atomic_fetch_add(&sent, 1);
    if (strstr(path, "ack"))
        atomic_fetch_add(&acks, 1);
    return true;
}
bool talk_rtsp_message_bytes(uint32_t ip, const char *path, const char *type, const void *data, size_t size) {
    (void)ip;
    (void)path;
    (void)type;
    (void)data;
    (void)size;
    return true;
}
static const esp_partition_t part = {.address = 0x930000, .size = 0x6d0000};
const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *name) {
    (void)type;
    (void)subtype;
    assert(!strcmp(name, "user_data"));
    return getenv("STORE_MISSING") ? NULL : &part;
}
int esp_partition_read(const esp_partition_t *p, size_t offset, void *data, size_t length) {
    (void)p;
    (void)offset;
    memset(data, 0xff, length);
    return 0;
}
int esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *cfg) {
    mkdir(cfg->base_path, 0700);
    return 0;
}
int esp_spiffs_info(const char *name, size_t *total, size_t *used) {
    (void)name;
    *total = getenv("STORE_FAIL") ? 4096 : 0x6d0000;
    *used = 0;
    return 0;
}
static void wait_idle(void) {
    usleep(200000);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    message_store_init();
    if (!strcmp(argv[1], "missing")) {
        assert(!message_store_ready());
        size_t total = 1, used = 1;
        message_store_usage(&total, &used);
        assert(!total && !used);
        puts("Missing filesystem reported unavailable");
        return 0;
    }
    assert(message_store_ready());
    size_t total, used;
    message_store_usage(&total, &used);
    assert(total == (getenv("STORE_FAIL") ? 4096 : part.size) && !used);
    flash_message_init();
    ui_inbox_t inbox;
    flash_message_snapshot(&inbox);
    if (!strcmp(argv[1], "seed")) {
        assert(!inbox.count);
        uint32_t peer = inet_addr("127.0.0.2");
        assert(flash_message_receive(
            peer, "/messages", "{\"id\":\"0123456789abcdef\",\"sender\":\"Other knob\",\"text\":\"Tea is ready\"}"));
        assert(flash_message_receive(
            peer, "/messages", "{\"id\":\"0123456789abcdef\",\"sender\":\"Other knob\",\"text\":\"Tea is ready\"}"));
        flash_message_snapshot(&inbox);
        assert(inbox.count == 1);
        assert(flash_message_receive(
            peer, "/messages", "{\"id\":\"1123456789abcdef\",\"sender\":\"Other knob\",\"text\":\"Second message\"}"));
        assert(flash_message_ack("0123456789abcdef"));
        wait_idle();
        flash_message_snapshot(&inbox);
        assert(inbox.messages[0].acknowledged && atomic_load(&acks) == 1);
        assert(flash_message_ack("0123456789abcdef"));
        wait_idle();
        assert(atomic_load(&acks) == 1);
        char id[17];
        assert(flash_message_send(peer, "Reply", id));
        wait_idle();
        char ack[80];
        snprintf(ack, sizeof(ack), "{\"id\":\"%s\"}", id);
        assert(flash_message_receive(peer, "/messages/ack", ack));
        assert(!flash_message_receive(peer, "/messages", "{\"id\":\"bad\",\"sender\":\"x\",\"text\":\"x\"}"));
        assert(!message_store_write("../escape", "x", 1));
    } else if (!strcmp(argv[1], "reload")) {
        assert(inbox.count == 2 && !strcmp(inbox.messages[0].text, "Tea is ready"));
        cJSON *json = flash_message_json();
        cJSON *out = cJSON_GetObjectItem(json, "outbox");
        assert(cJSON_GetArraySize(out) == 1);
        assert(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(out, 0), "acknowledged")));
        cJSON_Delete(json);
    } else if (!strcmp(argv[1], "full")) {
        assert(inbox.count == 2);
        uint32_t peer = inet_addr("127.0.0.2");
        assert(!flash_message_receive(peer, "/messages",
                                      "{\"id\":\"2123456789abcdef\",\"sender\":\"Other knob\",\"text\":\"No room\"}"));
        flash_message_snapshot(&inbox);
        assert(inbox.count == 2);
        char id[17];
        assert(!flash_message_send(peer, "Cannot queue without durable storage", id));
        assert(atomic_load(&sent) == 0);
    } else if (!strcmp(argv[1], "voice")) {
        extern atomic_uint output_frames;
        voice_notes_init(); char id[17]; assert(voice_notes_record(0, id)); usleep(500000); voice_notes_stop();
        ui_voice_t state;
        for (int i=0;i<300;i++) { voice_notes_get_state(&state); if (state.state==UI_VOICE_IDLE) break; usleep(10000); }
        assert(state.state==UI_VOICE_IDLE);
        size_t length; uint8_t *ogg=voice_notes_read(id,&length); uint32_t duration;
        assert(ogg&&voice_ogg_validate(ogg,length,&duration)&&duration>=400&&duration<=800);
        assert(voice_notes_play(id));
        for (int i=0;i<300&&!atomic_load(&output_frames);i++) usleep(10000);
        assert(atomic_load(&output_frames)>0); voice_notes_stop();
        for (int i=0;i<300;i++) { voice_notes_get_state(&state); if (state.state==UI_VOICE_IDLE) break; usleep(10000); }
        assert(state.state==UI_VOICE_IDLE);
        assert(voice_notes_receive(inet_addr("127.0.0.2"),"Remote","3123456789abcdef",ogg,length));
        assert(voice_notes_receive(inet_addr("127.0.0.2"),"Remote","3123456789abcdef",ogg,length));
        ogg[length-1]^=1; assert(!voice_notes_receive(inet_addr("127.0.0.2"),"Remote","4123456789abcdef",ogg,length)); free(ogg);
        assert(flash_message_delete(id)); usleep(200000); assert(!voice_notes_read(id,&length));
    } else if (!strcmp(argv[1], "fallback")) {
        assert(inbox.count >= 1 && inbox.count <= 2 && !strcmp(inbox.messages[0].text, "Tea is ready"));
    } else
        abort();
    printf("Inbox %s passed\n", argv[1]);
}

void talk_persist_settings(void) {}
