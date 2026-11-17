Run `python3 tests/lan_voice/run.py` from any directory. It requires clang.

The checks compile production code with address and undefined-behaviour
sanitizers. Host FreeRTOS adapters live only in this directory. Tests cover
RTP framing and jitter loss/wrap, shared AirPlay request parsing, RTSP
client/server call setup and busy rejection, Talk source handovers with OTA
suspension, and background Talk volume routing. The RTSP check uses an
available local TCP port and sends no traffic to physical AudioKnobs.
