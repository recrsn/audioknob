# LAN Talk

Hold-to-talk between AudioKnobs on the same network. There are no text
messages, voice notes or inbox: speech is live only.

## Device UI

- **Talk:** choose a discovered knob and hold the centre button to speak. Release
  to listen. A level ring shows speech, and the knob changes voice playback volume.
  Incoming speech pauses the current source, plays a brief tone and opens Talk.
  After it ends, the sender stays selected for a reply. Knob and web volume
  also control voice playback while another page is open.

## HTTP API

| Request | Result |
| --- | --- |
| `GET /api/talk` | Discovered `peers` (`ip`, `name`), state, current peer, volume |

Talk states: 0 idle, 1 connecting, 2 sending, 3 receiving.

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

Talk is its own `audio_source` owner. It pauses/preempts the previous source and
yields before another writer can write PCM. OTA suspension stops it too.
Playback never automatically resumes the displaced source. The microphone stays
a reusable single-owner module, with the default +30 dB gain and mild ESP-SR
suppression. Codec state and codec task stacks are in PSRAM; the Talk volume is
saved (debounced 2 s) from the esp_timer task, never from the codec tasks.

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
stand-ins for FreeRTOS, under ASan/UBSan. They do not prove ESP codec timing,
mic capture or WiFi performance on hardware.

The native preview has sample peers. `T` toggles incoming Talk; hold the centre
button to simulate outgoing voice.
