#pragma once

#include <stddef.h>
#include <stdint.h>

// The reply to one POST /fp-setup body (malloc'd), or NULL if unrecognised.
uint8_t *fp_setup_reply(const uint8_t *req, size_t req_len, size_t *reply_len);
