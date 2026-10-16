#pragma once

/*
 * The event channel: a TCP port the receiver opens at SETUP, which the
 * sender connects to and the receiver then sends requests over (encrypted
 * like the control channel). After shairport-sync's ap2_event_receiver.c
 * and remote.c.
 */

#include <stdbool.h>
#include <stdint.h>

#include "bplist.h"
#include "conn.h"

// Starts listening; the sender is told *port in the SETUP reply.
event_channel_t *event_open(const ap2_conn_t *conn, uint16_t *port);
void event_close(event_channel_t *ev);

// POST /command with this plist; true on a 2xx reply.
bool event_post_command(event_channel_t *ev, bpw_t *w, bpw_obj root);
