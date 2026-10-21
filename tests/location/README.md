Run `python3 tests/location/run.py` from the repository root. Requires a C compiler
with AddressSanitizer/UndefinedBehaviorSanitizer and Node.js.

The C harness executes the real `online/online.c` jobs through `net/network_jobs.c`, using a deterministic FreeRTOS
clock, scripted geolocation/weather responses and transactional in-memory NVS.
It checks cached automatic and manual locations across simulated reboots, no
automatic refresh over 24 hours, missing-location backoff (30 seconds through
the 10-minute cap), explicit refresh with cached coordinates, requests during
backoff and in-flight lookup, manual changes during lookup or flash write,
failed NVS open/write/commit and manual-pin removal, 15-minute weather and
optional air-quality timing, weather-failure backoff, offline persistence,
clock/network readiness and response-allocation retry. Selected coordinates
and location timezone fields are checked without changing timezone application.

The JavaScript harness executes the page's location renderer and click handlers,
checking visibility, automatic-cache reuse, explicit detection and failed requests.
The firmware builds validate the Settings action and HTTP handler integration;
these host tests do not exercise networking, LVGL rendering or physical flash.

The API remains compatible: `POST /api/location` with `auto=1` selects the saved
automatic location, and existing city/lat/lon/tz forms pin a manual location.
`detect=1` explicitly refreshes the detected cache without clearing a manual pin.
Both Settings screens offer “Detect again” while automatic location is selected.
