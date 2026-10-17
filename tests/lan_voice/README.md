Run `python3 tests/lan_voice/run.py` from any directory. It requires clang,
Homebrew libopus, ffprobe and ffmpeg, plus the S3 managed cJSON dependency
(downloaded by the PlatformIO S3 build).

The checks compile production code with address and undefined-behaviour
sanitizers. Host FreeRTOS/flash adapters live only in this directory. Tests cover
RTP framing and jitter loss/wrap, Ogg framing and real Opus decode, shared AirPlay
request parsing, RTSP client/server setup, concurrent JSON/binary delivery,
persistent inbox/outbox, duplicate delivery, manual ack, full storage and recovery
from an incomplete metadata generation. The RTSP check uses an available local
TCP port and sends no traffic to physical AudioKnobs.

The voice-note task check uses the real production record/storage/playback code
with a host libopus adapter and a synthetic mic. It also checks voice delivery,
duplicate suppression, CRC rejection and deletion of the stored audio file.
