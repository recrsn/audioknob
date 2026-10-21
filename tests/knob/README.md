Run `python3 tests/knob/run.py` on the macOS development host. It builds the
native LVGL library, then links the production GPIO encoder driver against it
with simulated timestamped pin edges and ASan/UBSan on the driver/test code.
No window or physical GPIO is used.

Checks ordered direction pulses, bounce and reversal rejection, short presses,
long holds with no release click, release-turn suppression, delayed LVGL reads,
queue overflow (including during a press), recovery and millisecond wraparound.
The input sink must remain non-editable and non-scrollable: LVGL's encoder edit
mode otherwise changes long-press/release semantics. The native LVGL archive
itself is built with the project's normal native flags, without sanitizers.

Board validation must cover fast spins, direction changes, button bounce and
input while rendering, networking and audio are busy. Interrupt delivery can be
delayed while flash cache is disabled; this driver does not promise lossless
input during flash erase/write operations.
