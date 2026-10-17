# LAN Talk, messages and voice notes

This feature is on `codex/push-to-talk`. The microphone gain and mild Espressif
suppression have been verified on Ami-Desk and landed on main. The Talk/inbox
firmware has **not been installed or tested on the boards**; it still needs the
hardware checks below before a fast-forward to main.

## Device UI

- **Talk:** choose a discovered knob and hold the centre button to speak. Release
  to listen. A level ring shows speech, and the knob changes voice playback volume.
  Incoming speech pauses the current source, plays a brief tone and opens Talk.
  After it ends, the sender stays selected for a reply. Knob and web volume
  also control voice playback while another page is open.
- **Voice note:** on a selected Talk peer, hold “Hold for a voice note”. Releasing
  saves the recording locally and queues a send to that peer. The receiver gets
  an inbox item; it does not play automatically. Notes stop at 60 seconds or the
  recording size limit. Recordings shorter than 200 ms are discarded.
- **Inbox:** lists stored text messages and voice notes. Tap an item to read it or
  play its audio. **Acknowledge** sends a separate receipt back to its sender.
  Acknowledging is optional. Deleting removes the local copy. “Hold to record
  note” saves a local recording without sending it.

Sending is fire and forget: an API request returns after the send is queued,
without waiting for delivery or a human acknowledgement. The background sender
makes one attempt. A failed delivery is recorded in the outbox; it is not retried
silently. At most eight inbox items and eight recent outgoing receipts are kept.
An occupied inbox rejects new items rather than discarding existing ones.

## HTTP API

All POSTs use the web server's existing same-origin check and
`application/x-www-form-urlencoded` bodies. Peer addresses are IPv4 addresses
from `GET /api/talk`.

| Request | Result |
| --- | --- |
| `GET /api/talk` | Discovered `peers` (`ip`, `name`), state, current peer, volume |
| `POST /api/messages` `action=send&peer=192.168.1.50&text=Tea%20is%20ready` | `202 {"id":"…","status":"queued"}`; text is 1–160 UTF-8 bytes |
| `GET /api/messages` | Persistent `inbox` and `outbox`; each has `id`, `peer`, `sender`, `text`, `pending`, `delivered`, `failed`, `acknowledged`, `voice`, `local`, `durationMs` |
| `POST /api/messages` `action=ack&id=…` | Queues a manual acknowledgement for a received item |
| `POST /api/messages` `action=delete&id=…` | Queues removal of a local inbox item and its audio, if any |
| `POST /api/voice` `action=record` | Begins a local recording and returns its ID with `202` |
| `POST /api/voice` `action=record&peer=192.168.1.50` | Records, then queues a voice note for that peer |
| `POST /api/voice` `action=stop` | Stops recording or playback |
| `GET /api/voice` | Recording/playback state, current ID, elapsed seconds, storage status and usage |
| `GET /api/voice?id=…` | Downloads a saved voice note as `audio/ogg` |
| `POST /api/voice` `action=play&id=…` | Queues playback on this knob |
| `POST /api/voice` `action=send&id=…&peer=192.168.1.50` | Queues a saved voice note for a peer |

Talk states: 0 idle, 1 connecting, 2 sending, 3 receiving. Voice-note states:
0 idle, 1 recording, 2 saving/loading, 3 playing. IDs are sixteen lowercase hex
characters. Acknowledgement is distinct from delivery: `delivered` means the
receiver saved the item; `acknowledged` means someone explicitly acknowledged it.

## Protocol and audio

Discovery uses the existing mDNS instance: `_audioknob._tcp`, TCP 42424, TXT
`v=1` and `name=<device name>`. AirPlay remains responsible for `mdns_init()`.
The Talk advertisement is added once after initial AirPlay probing; renames
update its TXT value rather than repeatedly adding the service.

Call setup uses RTSP/1.0 `ANNOUNCE` (SDP), `SETUP` (unicast UDP Transport), and
`RECORD` (`Session`, `RTP-Info`). It reuses AirPlay's `http_parse_message()`,
`http_send()`, header helpers, socket binder and RTP-Info parser. A closed control
connection ends the hold; the receiver drains a short tail. `TEARDOWN` is also
accepted. Sessions stop after 60 seconds, lost control, preemption, or 500 ms
without accepted audio. This is a separate service and does not enter AirPlay's
pairing or RAOP sessions.

Voice uses Opus, 16 kHz mono PCM, 20 ms packets, approximately 24 kbit/s VBR.
RTP uses dynamic payload 96 and the RFC 7587 48 kHz timestamp clock (960 ticks per
packet), with `opus/48000/2` in SDP and mono preferences in fmtp. UDP port 42425
receives media. An eight-packet reorder window starts after 60 ms of buffering;
missing frames use Opus PLC. The receiver resamples to the DAC's fixed 44.1 kHz
stereo format and uses the existing cubic, ramped volume. RTCP and retransmission
are not implemented for these short holds.

Messages use short independent RTSP `SET_PARAMETER` connections, so they can
arrive while a voice session is active:

- `/messages`, `application/json`: `{id,sender,text}`.
- `/messages/ack`, `application/json`: `{id}`. The receiver of an ack matches both
  its ID and the original destination address.
- `/messages/voice?id=<id>`, `audio/ogg`: a saved mono Opus note, one 20 ms Opus
  packet per Ogg page. Sender name comes from `X-AudioKnob-Name`.

An RTSP 200 on a message means it was saved. It is not a manual acknowledgement.
Repeated IDs from the same sender are idempotent. Text requests are bounded to
1 KiB; voice notes to 300 KiB and 60 seconds. Ogg CRCs, page sequence, granules,
mono Opus headers and packet durations are checked before a note is accepted.

Talk and voice notes have distinct audio-source owners. Both pause/preempt the
previous source and yield before another writer can write PCM. OTA suspension
stops them too. Playback never automatically resumes the displaced source.
The microphone stays a reusable single-owner module, with the approved default
+30 dB gain and mild ESP-SR suppression. Codec state and codec task stacks are in
PSRAM; flash writes run on internal stacks, never on the codec tasks.

## Flash data

`user_data` occupies **0x930000–0xffffff**, size **0x6d0000 (6.8125 MiB)**.
Both S3 OTA slots are 4 MiB and the ESP32 staging partition is 1 MiB. See
[flash-layout.md](flash-layout.md) for the complete table and fresh-flash steps.
This layout requires a full erase and USB flash; application-only OTA cannot
migrate the old partition table. No runtime partitions are registered.

Settings → About → Storage used and the web page's About section report used
and total mounted filesystem capacity. `GET /api/state` exposes
`storageUsedBytes` and `storageTotalBytes` (zero total means unavailable).
The mounted capacity excludes SPIFFS overhead; writes also keep its last
quarter free for garbage collection.

The filesystem is SPIFFS. First use formats only a completely erased region.
A damaged or unknown non-erased region is preserved, with storage reported
unavailable. The last quarter is kept free for filesystem garbage collection.
Inbox and outbox metadata alternate between checksummed generations; boot falls
back to the other generation if one is incomplete. Saved audio uses `.ogg`
files. Data survives ordinary firmware OTA. A full-chip erase removes it.

## Checks

```sh
python3 tests/lan_voice/run.py
pio run -d firmware/s3 -e native
pio run -d firmware/s3 -e s3
```

The host checks use the production RTP/Ogg/RTSP/storage/message implementations
with POSIX stand-ins for FreeRTOS and flash. They run with ASan/UBSan, encode real
Opus, decode the generated Ogg with FFmpeg, exercise concurrent call/message
connections, and check restart, duplicate delivery, full storage and damaged
metadata recovery. The production voice-note tasks also record, save, play,
receive and delete notes using a host Opus adapter. They do not prove ESP codec timing, mic capture, filesystem
power-loss behaviour or radio/WiFi performance on hardware.

The native preview has sample peers and messages. `T` toggles incoming Talk;
hold the centre button to simulate outgoing voice, and use Inbox to check manual
acknowledgements. Native voice recording/playback is simulated.

Before merging: install the branch on Ami-Desk; check first-use data mount and
free internal RAM; exercise Talk against a host peer; record, play, send, ack and
delete notes; restart and verify saved data. Then test one direction between two
knobs, with user authorization before flashing Priya-Desk or using another board
supplied by the user. Recheck source preemption and OTA suspension on that build.
