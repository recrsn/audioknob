Run `python3 tests/network_jobs/run.py` from the repository root. A C compiler
with pthreads, AddressSanitizer and UndefinedBehaviorSanitizer is required.
The real dispatcher runs with pthread-backed FreeRTOS mutexes/tasks and a
controllable clock. It checks one invocation per pending kind, earliest deadline
coalescing, offline/unsynced gating, offline persistence, timer lock contention,
worker/timer allocation failures and recovery, requests during a callback,
round-robin fairness with self-resubmitting work, disconnect/reconnect, concurrent
producers, and notification at the boundary between publishing idle and blocking.
Callbacks assert they never execute concurrently; exactly one task is created
across repeated refreshes, and its static stack/TCB addresses stay fixed.

`python3 tests/location/run.py` exercises real weather/location/persistence jobs
through the dispatcher with transactional NVS and scripted responses, including
backoff and in-flight user changes. `python3 tests/ota/run.py` exercises release
jobs through it, readiness and OTA deferral, startup/manual discovery, no daily
polling, retained notices, allocation/HTTP failures, slow-body deadlines, timestamp
rendering and the existing OTA validation/download/confirmation behavior.

Host tests cannot validate ESP32 capability heaps, stack high-water marks, TLS
latency, timer task performance or audio quality during a background job. See
`docs/memory-budget.md` for static accounting and the board-validation plan.
