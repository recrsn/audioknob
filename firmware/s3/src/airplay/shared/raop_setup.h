#pragma once

/*
 * AirPlay 1 session setup shared by both receivers: the Apple-Challenge
 * response, the RSA-wrapped AES key and SDP from ANNOUNCE, and the
 * SETUP/RECORD/FLUSH headers.
 *
 * Ported from philippe44's squeezelite-esp32 raop component
 * (c) Philippe 2019-2020, philippe_44@outlook.com -- MIT License.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lwip/inet.h"

// Parses the AirPort Express key; call once before the others.
bool raop_setup_init(void);

// Apple-Response for a base64 Apple-Challenge, signed over the address the
// sender reached us on and our MAC. Heap-owned, or NULL.
char *raop_auth_challenge(const char *challenge, struct in_addr local, const uint8_t mac[6]);

// Unwraps ANNOUNCE's a=rsaaeskey.
bool raop_auth_aes_key(const char *b64, uint8_t out[16]);

// Base64 without the padding Apple senders leave out. Bytes written, 0 on error.
size_t raop_base64_decode(const char *src, uint8_t *dst, size_t dst_size);

// Value of an SDP "a=<name>:" attribute, up to the line ending. Heap-owned.
char *raop_sdp_attr(const char *body, const char *name);

// Transport header's control_port and timing_port; false if either is missing.
bool raop_parse_transport(const char *transport, uint16_t *control_port, uint16_t *timing_port);

// RTP-Info's seq and rtptime, 0 where absent (or the header is NULL).
void raop_parse_rtp_info(const char *rtp_info, uint16_t *seqno, uint32_t *rtptime);
