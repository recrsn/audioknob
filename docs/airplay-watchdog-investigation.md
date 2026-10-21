# AirPlay 2 watchdog investigation — 2026-10-21

Checkpointed the control-stack diagnostics as `539386e`, then added temporary
buffered-decoder diagnostics and USB-flashed `v0.1.6-4-g539386e-dirty`.
Priorities, task placement and intentional sleeps are unchanged. Counters time
AAC calls, reads and CPU-0 idle gaps. An idle hook records its last execution;
a fixed eight-entry allocation-failure ring records request size, capabilities,
free/largest matching heap and task. No per-packet diagnostic logging occurs;
summaries print at decoder exit. Measurements include diagnostic overhead and
wall-clock AAC times include preemption, rather than measuring codec CPU alone.

## Saved logs and pacing difference

Five previously saved serial logs were searched; no recorded watchdog named
`radio_dec`. The original warning named `ap2_buf_dec` and starved `IDLE0`, about
5.5 seconds after buffered-stream startup, against a five-second watchdog.
The retained radio stream identifiable in these logs is MP3, so this is not a
claim that every radio AAC workload has been exhaustively checked.

Radio and AirPlay use separate tasks/decoder instances and the same AAC library
(the radio simple-decoder wrapper also handles framing and HE-AAC). Both tasks
run at priority 7 on CPU 0. Radio normally blocks in `audio_out_write()` /
`i2s_channel_write()`. Buffered AirPlay copies PCM to the player queue, which
returns without waiting for DAC playback. Its decode/drop/flush paths need not
block when data is available and it has not reached its target decode lead.

`stream_player_stop()` invalidates radio output and notifies `radio_net`
(priority 5), but does not directly set `s_decode_quit`. The network task sets
that flag only after its streaming session returns. Meanwhile `radio_dec` can
keep decoding queued bytes, and its invalid-output check returns before its
usual blocking I2S write. This creates a concrete handoff scheduling hazard:
two ready priority-7 decoders can delay lower-priority radio teardown and idle.

## On-device comparison

Native Apple Music supplied transient-paired, PTP, buffered AirPlay 2 sessions.
First started AirPlay with no radio decoder, then started it over station 0
(SomaFM Groove Salad MP3). Both sessions decoded without codec errors.

| Measurement | Clean AirPlay start | Radio → AirPlay |
| --- | ---: | ---: |
| Decoded packets | 3,769 | 3,914 |
| Longest AAC call, wall time | 23,652 µs | 40,455 µs |
| Total AAC call wall time | 23,519,366 µs | 25,598,400 µs |
| Maximum observed CPU-0 idle gap | 644,135 µs | 3,156,197 µs |
| Packets skipped as already late | 23 | 37 |
| Failed allocation attempts | 0 | 1 |
| Decoder errors | 0 | 0 |

Neither diagnostic run reproduced the original five-second watchdog warning.
The three-second idle gap and short measured AAC calls support starvation
during handoff, rather than one multi-second AAC call in these runs. They do
not establish which task consumed every interval of the original warning.

## Reproduced AES allocation failure

During the radio → AirPlay test the failure hook captured:

- Task: `radio_net`; allocator: `heap_caps_aligned_alloc`.
- Request: **1,254 bytes**, capabilities `0x8` (`MALLOC_CAP_DMA`).
- Matching heap free: **2,143 bytes**; largest matching block: **864 bytes**.
- Callback timestamp: 167,180 ms since boot; the AES/TLS errors logged later.

This directly establishes fragmentation of the required DMA-capable heap at
that instant: sufficient aggregate matching bytes, no contiguous block large
enough. Total internal heap also reached a low of 4,644 bytes during this
handoff. The ESP-IDF AES external-buffer path allocates DMA bounce buffers
and emits the observed error on allocation failure. This request came from
the old radio TLS/network worker, not from the AAC decoder. The exact AES
request is smaller than an assumed full TLS-record bounce buffer; the hook
measurement is authoritative for this run.

## Fix targets and remaining validation

1. Make radio decoder cancellation independent of lower-priority network
   teardown, including rapid play/stop/restart races. Stop decoding discarded
   radio backlog promptly after preemption.
2. Bound sustained buffered-AirPlay catch-up/drop work and give lower-priority
   tasks actual blocking opportunities. A plain taskYIELD cannot ensure idle.
3. Ensure sufficient contiguous DMA-capable memory during source overlap,
   including old TLS teardown. A shared decoder worker can reduce overlap and
   task churn, but does not by itself guarantee either pacing or DMA headroom.
4. Repeat handoffs with radio AAC and MP3, active HTTPS jobs, and pause/seek;
   verify zero watchdogs/allocation failures and measure idle gaps and audio
   underruns. Keep the watchdog enabled.

No scheduling or memory fix was applied in this investigation. The diagnostic
firmware remains on the device. Music was left paused with output on MacBook
speakers; station 0 resumed on the knob at its original volume 36.

Raw logs, state snapshots, exact diagnostic source, and firmware SHA-256:
/Users/amitosh/.codex/visualizations/2026/10/21/01a15cf6-fa38-7802-a51c-bdfeff74c601/airplay-watchdog-investigation
