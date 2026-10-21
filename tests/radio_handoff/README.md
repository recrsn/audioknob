Run `python3 tests/radio_handoff/run.py` with clang on the development host.

The ASan/UBSan lifecycle test runs the production stream player, compressed
ring and PCM helpers against pthread-backed FreeRTOS primitives, a fake codec,
and a socketpair-backed network source. It holds network cleanup deliberately
to verify that decoder cancellation does not depend on the network worker.
It covers shutdown of a blocked read, wait timeout, rapid stop/play, cancellation
before task startup, stopping during decode, stale callbacks, station switching,
and decoder-task creation failure/recovery. It does not validate codec quality.

A second test runs the production HTTP hop code with a fake HTTP client to
check socket publication after successful open, rejection after cancellation,
unpublication before HTTP cleanup, and failed-open cleanup.

Board checks use native Apple Music AirPlay 2 and HTTPS radio, preserving the
existing shared I2S DMA ring. See docs/radio-airplay-handoff.md for behavior and
captured results. These tests do not emulate FreeRTOS priority scheduling;
starvation and DMA-fragmentation checks require the board.
