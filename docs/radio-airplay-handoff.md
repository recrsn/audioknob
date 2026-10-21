# Radio → AirPlay handoff

2026-10-21. Investigation checkpoint: `94da666`. Fix tested as
`v0.1.6-5-g94da666-dirty` on the S3 via USB.

## Behavior

Radio stop immediately invalidates output and sets decoder cancellation, rather
than waiting for the lower-priority network worker to finish. The current codec
call may finish; returned PCM cannot reach the output after cancellation.
An invalid output epoch also makes a decoder waiting for a station switch sleep,
instead of decoding discarded backlog without its usual I2S backpressure.

The HTTP worker publishes its connected socket under the stream-player lock.
Stop and station change use shutdown to wake a blocked read. The worker
unpublishes the socket under that same lock before HTTP cleanup, so cancellation
cannot target a descriptor after cleanup closes it. TLS/client destruction stays
on the owning network task; cross-task HTTP cancellation is not used because
ESP-IDF's cancel_request API reconnects the client.

A new RAOP_EVENT_PREPARE callback runs for validated AirPlay audio SETUP, before
AirPlay 1 or 2 stream workers/buffers are allocated. It stops active radio and
waits up to 1,000 ms for its decoder, HTTP/TLS client and compressed ring to
finish. The successful SETUP reply, including audio ports, is sent after this
and stream initialization. Pairing, PTP timing setup and remote-control-only
SETUP do not request radio teardown.

**Audio SETUP can happen while Apple Music is paused.** Radio therefore stops
when the sender configures an audio stream, potentially before Play is pressed.
The app continues to mark AirPlay as not playing until accepted audio arrives.

If cleanup misses the deadline, audio SETUP returns 503 before allocating the
new stream. A new radio play racing with preparation also prevents successful
teardown. A radio connection still inside DNS/connect/TLS setup cannot be
interrupted through the not-yet-published socket; its existing 10-second connect
timeout remains. In that case SETUP may be refused after the one-second wait.
Kernel task-stack/TCB reclamation can follow task exit through idle cleanup.

Rapid stop/play preserves cancellation of the old lifecycle and restarts a fresh
one after it closes. Late audio/title callbacks cannot reactivate a stopped or
superseded stream epoch.

## Shared DMA output

No new DMA ring was added. audio_out_init creates I2S1 once, with six buffers of
240 stereo 16-bit frames (5,760 payload bytes). Radio, AirPlay, ambience and mixed
chimes already share this persistent output and its write mutex. Source changes
retain that allocation. Bluetooth on the other MCU has separate memory/output.

## Validation

- S3 firmware build and USB upload passed.
- tests/radio_handoff/run.py passed under ASan/UBSan: blocked socket shutdown,
  decoder exit while network cleanup is held, teardown timeout, stale callbacks,
  rapid restart, stop before task startup, cancellation during decode, station
  change, decoder-create failure/recovery, and HTTP socket publication/cleanup.
- Existing LAN voice/source arbitration and sound lifecycle checks passed.
- Native Apple Music supplied a PTP buffered AirPlay 2 stream after HTTPS MP3
  radio. Radio was also restarted during the retained AirPlay session, then
  Music resumed over it. The app was paused and its original selected output
  restored after testing.

The buffered decoder summary covered 6,506 AAC calls, with zero late packets,
zero codec errors, and zero recorded allocation failures. Maximum AAC-call wall
time was 24,899 µs; maximum observed CPU-0 idle gap was **523,233 µs**, versus
3,156,197 µs in the prior unfixed radio handoff. No watchdog or AES allocation
failure was observed in this run. Ordinary TLS/socket-closed error messages are
expected when shutdown interrupts an old connection; no reconnect was attempted
for the cancelled generation. These are bounded regression runs, not proof of
all station/sender/workload combinations.

HTTPS AAC radio (Radio Paradise) also reached playback and was handed off to
an AirPlay 1 tone stream successfully. A separate MP3-radio → AirPlay 1 check
passed. Station 0 was restored afterward at its original volume 36.

The control task's historical high-water estimate reached 3,068 bytes during
these tests, including diagnostics. Its 6 KiB reservation remains unchanged.
No crypto scratch-pool, Wi-Fi allocation, LCD placement, or decoder-worker
consolidation changes were made.

Raw logs, states, test output and exact firmware hashes are saved in the task's
radio-airplay-handoff artifact directory.

Protocol reference: [Shairport Sync audio SETUP handling](https://github.com/mikebrady/shairport-sync/blob/master/rtsp.c#L2646-L2682) initializes the buffered receiver and returns its data/control ports in the SETUP reply.
