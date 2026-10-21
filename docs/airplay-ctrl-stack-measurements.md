# AirPlay control stack measurement — 2026-10-21

Committed encoder checkpoint `3cb190b` and rebased onto main `d110ad4`
(already up to date). Built and flashed the diagnostic S3 firmware over USB
(`/dev/cu.usbmodem1101`, MAC D0:CF:13:1E:14:48). Running version:
`v0.1.6-3-g3cb190b-dirty`. The stack remains 6,144 bytes.

`ctrl_stack_log()` reports ESP-IDF's byte-valued stack high-water mark after
startup and each control operation. Peak use is reserved minus the minimum
untouched bytes: a historical estimate, not the isolated cost of each operation.
Later samples include diagnostic logging overhead and are conservative for a
build without that logger. Stack fill alone cannot prove all future paths fit.

| Observation | Minimum untouched bytes | Estimated peak used bytes |
| --- | ---: | ---: |
| mDNS startup | 3,908 | 2,236 |
| Receiver creation | 3,684 | 2,460 |
| First DACP volume request | 3,572 | 2,572 |
| Maximum during subsequent volume/control exercise | 3,396 | 2,748 |

The Mac streamed a quiet 440 Hz tone with pyatv's AirPlay 1 sender and advertised
a synthetic DACP endpoint. Real HTTP volume, play/pause, next and previous
requests from the knob received 204 responses. Device rename/restore exercised
mDNS and receiver name updates. Delayed replies tested the two-second receive
timeout. Ambience preemption tested failed remote pause and receiver abort.
Playback was stopped and the original name restored. No panic, assertion or
reboot was observed. This is a synthetic-sender control-path exercise, not a
long-duration test against every real-world sender or physical knob QA.

2 KiB is below observed use in this instrumented build. At 3 KiB only 324 bytes
would remain after the observed maximum. Keep 6 KiB for now; 4 KiB is a candidate
for subsequent stress testing, with 1,348 bytes relative observed headroom.
No stack reservation was reduced during this measurement.

Raw serial logs, HTTP exercise output/script, state snapshots and summary:
/Users/amitosh/.codex/visualizations/2026/10/21/01a15cf6-fa38-7802-a51c-bdfeff74c601/airplay-ctrl-stack

Flashed firmware.bin SHA-256: `5d4d44cf68d59ac6bbacc224685d906d5e1d663dc2695c585b831fc25e80c9f0`.


## AirPlay 2 with Apple Music

A follow-up used the Mac's native Apple Music app, streaming “Atlas, Rise!”
and briefly moving to the next track and back. USB logs confirm transient
pairing, a PTP session from `titanium`, and a buffered stream on connection 3.
This was a real AirPlay 2 session, not pyatv's AirPlay 1 or synthetic DACP test.
The run used the same diagnostic firmware without a reflash or stack resize.

| AirPlay 2 observation | Minimum untouched bytes | Estimated historical peak bytes |
| --- | ---: | ---: |
| Volume commands | 3,284 | 2,860 |
| First play/pause | 3,204 | 2,940 |
| Second play/pause and subsequent controls | 3,124 | **3,020** |

Volume changes (including 50→5→40→50), pause/resume, next/previous,
rename/restore, and pause on radio preemption completed over AirPlay 2's
MediaRemote path. Both metadata and decoded cover art arrived. The stack
high-water mark is historical over this boot, and includes diagnostic logging.
The higher maximum was observed during the AirPlay 2 pause/resume sequence.
The prior 2,748-byte measurement only covered AirPlay 1 controls.

A 3 KiB stack would now leave only **52 bytes** relative to the observed
maximum. A 4 KiB candidate would leave **1,076 bytes**, before any unobserved
paths or workload variation. Keep the current 6 KiB until a deliberate smaller
build has been stress-tested. 2 KiB is below measured usage.

### Decoder watchdog warning

One nonfatal CPU 0 idle watchdog warning occurred near buffered-stream startup,
while `ap2_buf_dec` was running. Symbolization of the captured backtrace points
through `unpack_idx_esc` → AAC Huffman/decode functions →
`esp_aac_dec_decode` → `decoder_decode` (`shared/decoder.c:170`) →
`play_block` / `decoder_task` (`v2/buffered.c:279,359`). Playback and controls
continued afterward; this was not an `airplay_ctrl` stack overflow. Startup
also logged timestamp gaps and late-packet trimming. The cause of decoder CPU
starvation has not been fixed or established by this control-stack test.

After the test, Apple Music was left paused on the original track with output
restored to MacBook speakers, and the knob's original radio station 0 resumed
at its original volume 36. The original device name was restored. No reset was
observed. Raw logs, before/active/after state, and per-operation readings:
/Users/amitosh/.codex/visualizations/2026/10/21/01a15cf6-fa38-7802-a51c-bdfeff74c601/airplay-ctrl-stack/airplay2


A subsequent handoff-fix regression run reached **3,068 bytes** of historical
control-stack use (diagnostics included). See [radio-airplay-handoff.md](radio-airplay-handoff.md).
