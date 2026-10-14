#pragma once

/*
 * The UDP side of a stream: the control port every stream gets, and for
 * realtime streams (type 96) the audio port, with ALAC packets the sender
 * paces in real time ~2 s ahead and anchors through the control port.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct rtp rtp_t;

// *data_port is only set (and audio only received) when realtime.
rtp_t *rtp_start(const uint8_t key[32], bool realtime, uint16_t *data_port, uint16_t *control_port);
void rtp_stop(rtp_t *r);
