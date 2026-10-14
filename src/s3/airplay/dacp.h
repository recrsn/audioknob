#pragma once

/*
 * DACP remote control back to an AirPlay 1 sender: find its _dacp._tcp
 * service by the DACP-ID it sent, then send it /ctrl-int commands. One
 * remote at a time, as there is one session at a time.
 *
 * Ported from philippe44's squeezelite-esp32 raop component
 * (c) Philippe 2019-2020, philippe_44@outlook.com -- MIT License.
 */

#include <stdbool.h>

void dacp_init(void);

// Starts looking for the sender's remote, replacing the previous one. Either
// argument NULL just forgets the old remote.
void dacp_start(const char *dacp_id, const char *active_remote);
// Bounded by an in-flight mDNS query (~3 s).
void dacp_stop(void);

// "playpause", "nextitem", "setproperty?dmcp.device-volume=-12.0000", ...
// Blocking network I/O; false if no remote has been found.
bool dacp_command(const char *command);
// This speaker's own volume as the sender shows it, 0..1.
bool dacp_volume(float volume);
