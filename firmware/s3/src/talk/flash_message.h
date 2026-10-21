#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "ui/ui.h"

void flash_message_init(void);
// Queues once and returns immediately; ID is 16 hex digits plus NUL.
bool flash_message_send(uint32_t peer, const char *text, char id[17]);
bool flash_message_ack(const char *id);
bool flash_message_delete(const char *id);
bool flash_message_receive(uint32_t peer, const char *path, const char *json);
bool flash_message_add_voice(uint32_t peer, const char *sender, const char *id, uint32_t duration_ms, bool local);
bool flash_message_send_voice(uint32_t peer, const char *id);
void flash_message_snapshot(ui_inbox_t *out);
cJSON *flash_message_json(void);
