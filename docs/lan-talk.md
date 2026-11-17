# LAN Talk

Two-way calls between AudioKnobs on the same network. Speech is live only:
there are no text messages, voice notes or inbox.

## Device UI

- **Calling:** Talk lists the knobs it has discovered. Opening one shows its
  avatar and name over Back and a green Call button. While the other knob
  rings, rings pulse out of the avatar, it reads "Calling…", and a red End
  button replaces the pair. Busy, Declined and No answer show under the name
  if the call doesn't connect.
- **Ringing:** an incoming call wakes the display, opens Talk on the caller
  with the same pulsing avatar, and plays a ring over whatever is playing,
  which ducks under it. Decline (red, handset down) and Accept (green) sit side
  by side. Nothing is captured until the user accepts; after 30 s the call
  rings out as missed.
- **In a call:** both sides talk at once. The line under the name shows the
  call's time, and the knob sets the call's volume. Either side hangs up with End. The call keeps
  going when you leave the page; the Talk band item on the clock and the
  launcher's status show it.

Answering takes the DAC: Bluetooth is paused, and AirPlay and radio are
stopped. Nothing resumes on its own when the call ends.

## HTTP API

| Request | Result |
| --- | --- |
| `GET /api/talk` | Discovered `peers` (`ip`, `name`), `state`, `peer`, `seconds` in the call, `volume` |

Talk states: 0 idle, 1 calling, 2 ringing, 3 in a call.

## Protocol

Discovery uses the existing mDNS instance: `_audioknob._tcp`, TCP 42424, TXT
`v=2` and `name=<device name>`. Knobs advertising `v=1` (the old hold-to-talk
firmware) aren't listed. AirPlay remains responsible for `mdns_init()`; the
Talk advertisement is added once after its initial probing, and renames update
the TXT value.

A call is one RTSP/1.0 TCP connection from caller to callee, using AirPlay's
`http_parse_message()`, `http_send()`, header helpers, socket binder and
RTP-Info parser:

1. `ANNOUNCE` with SDP: `opus/48000/2`, `a=ptime:20`, `a=sendrecv` (required).
2. `SETUP` with `Transport: RTP/AVP/UDP;unicast;client_port=42425`; the reply
   carries `Session` and `server_port`.
3. `RECORD` with `RTP-Info` (`seq`, `rtptime`). The callee rings and replies
   only once its user decides: `200` with its own `RTP-Info`, `603 Decline`,
   `480 Temporarily Unavailable` after 30 s of ringing, or `453` straight away
   when it is already ringing or in a call.
4. The call lasts as long as the connection. Closing it (or `TEARDOWN`) hangs
   up, from either side; closing it while RECORD waits cancels the ring.

Each side sends RTP from and to UDP 42425, and accepts packets only from the
other side's address and negotiated port, locked to the first SSRC it sees.
A call ends after 3 s with no audio from the other side. There is no
encryption or authentication; the accept press on the callee is the gate.

## Audio

Opus, 16 kHz mono, 20 ms packets, about 24 kbit/s VBR at complexity 0. RTP
uses dynamic payload 96 and the RFC 7587 48 kHz timestamp clock (960 ticks per
packet). Measured per 20 ms frame on the knob: Opus ~5-7 ms, against ~2 ms for
uncompressed L16 and ~6 ms for esp_audio_codec's G.722; L16's 256 kbit/s lost
far more on a congested 2.4 GHz link.

The receiver keeps a 16-packet reorder window, started at the first packet
that arrives, and an adaptive playout delay: 60 ms at first, growing by
however many frames late a packet came (WiFi holds frames back in bursts of
up to ~350 ms), to at most 200 ms, by inserting concealed frames. After 10 s
with nothing late it shrinks by a frame, dropping one that follows a quiet
(< -45 dBFS) frame. Missing and inserted frames use Opus PLC. A sender whose
clock runs fast overflows the window and loses a frame. RTCP and
retransmission are not implemented: on a LAN the frames that miss their slot
are late, not lost, and a resend would come no sooner.

The mic paces the call: each 20 ms the worker reads a mic frame, encodes and
sends it, then decodes the next received frame and applies the volume. The
mic's processing runs on its own task on core 0. Decoded frames go to a
playout task through a 100 ms ring, primed with 40 ms; it resamples them to
44.1 kHz stereo and writes them to the DAC, blocking while the DAC's 33 ms
queue is full. Written straight from the worker, a frame that came out late
let the DAC run dry; the ring takes up that jitter instead. The DAC runs off
the same crystal as the mic, so the ring stays as deep as it started.

The mic and speaker share an enclosure, so the capture runs ESP-SR's echo
canceller (`AEC_MODE_VOIP_LOW_COST`, filter length 4, state in PSRAM) before
the gain: DC removal → AEC → +30 dB → mild noise suppression. The worker feeds
it the far end exactly as the DAC gets it (`mic_reference()`, after the
volume). The reference is written just before the DAC queue it waits behind,
so it leads the echo by that queue's depth (up to ~33 ms) plus the speaker's
path, within the filter's reach.

Talk is its own `audio_source` owner. It yields before another writer can
write PCM, and OTA suspension stops it too. Codec and echo canceller state and
the worker's task stack are in PSRAM. While a call rings or runs, the worker
switches to a 27 KB internal stack taken for that call
(`esp_execute_shared_stack_function()`; a call used ~24 KB, nearly all Opus),
only if 32 KB of internal RAM stays free besides; otherwise it runs on the
PSRAM stack, where a frame took ~25 ms. Internal allocations come out of the
same 32 KB `SPIRAM_MALLOC_RESERVE_INTERNAL` pool the LCD's SPI driver takes
its DMA bounce buffer from: a 30 KB stack once left the panel unable to draw.
Mic processing (AEC + NS) costs ~12-13 ms per 20 ms frame. Call audio
plays 6 dB below the Talk volume's curve, since the far end's +30 dB mic gain
puts speech near full scale; the ring plays at a fixed level (`CALL_GAIN`), not
the timer volume. The worker never writes flash, and the Talk volume is saved
(debounced 2 s) from the esp_timer task.

## Not verified on hardware

- How much echo the canceller removes on the knob's own speaker and mic, and
  whether `AEC_NLP_LEVEL_NORMAL` is enough or residual echo needs a stronger
  level or ducking the mic while the far end is loud.
- CPU headroom: a Mac-to-knob call puts the mic's processing (AEC ~8 ms per
  16 ms chunk beside WiFi, ~6 ms on core 1) at ~65% of core 0 and the worker
  (Opus, ~6.7 ms a frame) at ~32% of core 1, LVGL ~6%. With all of it on
  core 1 the worker took 80% and the DAC ran dry. Each call logs its tasks'
  CPU (`task_stats`), per-frame cost, DAC gaps, stack use and internal RAM at
  each setup step.
- Internal RAM in a call: ~30 KB free, largest block 21 KB. The call takes
  the 27 KB stack, 5.4 KB for the mic's I2S channel, ~2.3 KB for its
  processing and 8 KB for the processing task's stack. ESP-SR's echo canceller honours `caps` for only some buffers;
  the rest are plain mallocs below `SPIRAM_MALLOC_ALWAYSINTERNAL` (~28 KB),
  so `heap_caps_malloc_extmem_enable(0)` is held around its creation, at
  ~7% more AEC time. The noise suppressor and Opus take none.
- The board has no LCD TE (tearing effect) line to the S3, so flushes can't
  sync to the panel's refresh.
- The ring tone's level against playing music.

## Flash data

Talk keeps only its volume, in NVS namespace `talk`. The `user_data` SPIFFS
partition (0x930000, 6.8125 MiB) that held messages is no longer mounted; the
partition table keeps it so existing devices need no re-flash.

## Checks

```sh
python3 tests/lan_voice/run.py
pio run -d firmware/s3 -e native
pio run -d firmware/s3 -e s3
```

The host checks use the production RTP and RTSP implementations with POSIX
stand-ins for FreeRTOS, under ASan/UBSan: ringing, answer, busy, decline, the
caller giving up, and either side hanging up. They do not cover the codec,
echo cancellation, mic capture or WiFi.

The native preview has two sample knobs. Calling one answers after 2 s, then
the call's clock and level tick; `T` rings an incoming call (or ends one).
