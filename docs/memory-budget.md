# Radio internal-memory regression (static analysis)

## Evidence

After flashing the Talk/inbox build, the saved `/api/state` readings showed
15–18 KiB of free internal RAM and over 6 MiB free PSRAM. The earlier verified
radio firmware had about 40 KiB of free internal RAM. The user subsequently
reported running out of memory while starting radio. No failure log was
available for this analysis, so the exact failed allocation is unconfirmed.

The radio start path reserved two stacks explicitly from internal RAM:

| Allocation | Size | Lifetime |
| --- | ---: | --- |
| `messages` stack | 6 KiB | Always allocated after inbox initialization |
| `note_store` stack | 6 KiB | Always allocated after voice-note initialization |
| `radio_net` stack | 8 KiB | While a radio session is running |
| `radio_dec` stack | 8 KiB | While a radio session is running |
| Talk connection stacks | 6 KiB each, at most two | Only while connected |

Task control blocks, message queues, locks, SPIFFS buffers and network state
consume additional memory. Total free bytes do not guarantee a contiguous
8 KiB block for a task stack. The two radio task-creation failure paths both
report `Out of memory`.

The large radio ring, decoder PCM buffers, codec allocation hooks and LVGL
allocator already prefer/use PSRAM. The filesystem's cache and work-buffer sizes
are determined by `max_files` (5) and the 256-byte page size, rather than the
partition's capacity. Enlarging the flash partition does not allocate a
similarly sized RAM buffer.

The current sdkconfig also prefers internal memory for ordinary allocations up
to 16 KiB, with fallback to other eligible heaps. Thus small HTTP allocations
add pressure; explicit internal-only task stacks cannot fall back to PSRAM.

## Minimal fix

Keep each radio worker's 8 KiB stack depth, priority and core affinity, but
allocate both stacks in PSRAM with `xTaskCreatePinnedToCoreWithCaps`. Their exits
already call `vTaskDeleteWithCaps`. This removes a 16 KiB internal-stack demand
when starting radio; it does not change the idle heap reading.

The radio workers perform network reads, read-only TLS certificate handling,
codec operations and I2S output. They do not write partitions, NVS or files.
The project already enables external task stacks and uses them for Talk and
voice-note codec workers. Storage/message workers and incoming Talk connection
handlers still need internal stacks because they write flash.

No global malloc policy, partition layout, storage format, audio buffer size or
codec stack depth changes are needed for this fix.

## Verification limits

The S3 build checks the patched task-creation code against the configured IDF.
Host LAN voice checks cover source arbitration and persistence but cannot model
ESP32 capability heaps or prove radio performance with PSRAM stacks. MP3/AAC
startup, repeated source changes, largest free internal block and minimum free
internal heap still need board verification. No new device connection or flash
is required for the static analysis.

## On-demand ambience and system sounds

`focus_sound_init()` and `alarm_sound_init()` no longer start idle tasks.
Ambience retains its own worker, synth and `AUDIO_SOURCE_LOCAL` ownership.
Pomodoro, the Ambience UI and the web API still request it through the same
public interface. The worker keeps its existing core, priority, 4 KiB stack,
fades and write barrier, then exits after fade-out or source preemption.
Its synth and PSRAM render buffers are allocated on demand and freed on exit.

The existing alarm API feeds one separate `system_sound` worker, with its
existing core, priority and 3 KiB stack. An eight-entry, 32-byte command ring
holds alarm/preview/stop targets; all existing commands replace the current
sound, so the worker coalesces pending targets before mixing. Overflow discards
the oldest pending target, retaining the newest command (including stop).
Previews still cannot replace a repeating alarm. This dispatcher can accept
future system-sound kinds without merging ambience into it or adding a new UI.
The existing mix hook, ducking, repeat periods, preview/dismissal release and
S3 hold remain. The LUT is freed after unhooking, and the worker exits when
playback and queued work finish.

Both services serialize task publication and retirement with producers. A new
request either reaches the existing worker or creates a successor after all
old playback resources are released. No notifications target deleted tasks.
Task/buffer allocation failure logs a warning, clears failed intent and allows
a later request to retry; it does not abort firmware. Suspension cancels system
sounds and preempts ambience. A single atomic completion target prevents an
old mix completion from swallowing a replacement request.

### Static memory accounting (S3 build, not a board measurement)

| Change at idle | Payload bytes |
| --- | ---: |
| Remove persistent ambience + alarm stacks | +7,168 internal |
| Remove two task control blocks (`sizeof(StaticTask_t) == 340`) | +680 internal |
| Add two lifecycle mutexes (`sizeof(StaticQueue_t) == 84`) | -168 internal |
| Additional combined module `.data` + `.bss` | -51 internal |
| Free ambience synth (`sizeof(struct ambience_synth) == 1948`) | +1,948 from its ordinary malloc heap |
| Free ambience render buffers + alarm LUT | +7,172 PSRAM |

The stack saving alone, less new control storage, is **6,949 bytes (6.79 KiB)**.
Including task-control-block payload gives **7,629 bytes (7.45 KiB)** of nominal
internal savings. With the synth in internal memory (the configured preference
for ordinary allocations below 16 KiB), the nominal total is **9,577 bytes
(9.35 KiB)**; if that allocation previously fell back to PSRAM, count it there
instead. These subtotals exclude heap metadata and linker/allocation alignment.
Module data/BSS was compared by compiling HEAD and updated sources with the
same S3 compile commands: ambience 42 → 46 bytes, alarm 845 → 892 bytes. Type
sizes were inspected from the built ELF's debug information. The linker's
static RAM summary does not include the removed dynamic stacks.

FreeRTOS reclaims self-deleted task allocations from its idle task; heap
readings immediately after stop may therefore precede stack reclamation.
During playback, stacks keep their existing internal allocation capabilities
and sizes. External stacks are enabled in this project, but moving these
workers to PSRAM would be a separate cache/ROM and transitive-callback audit;
this change does not depend on that move, enable XiP or alter malloc policy.
The retained mix/synthesis control state remains small and reusable.

### Validation and board checks

`python3 tests/sound/run.py` compiles the real workers with pthread-backed
FreeRTOS/audio stubs under AddressSanitizer and UndefinedBehaviorSanitizer.
It checks idle initialization, create/buffer failures and retry, overflow and
stop retention, preview gating/completion, stale completion versus replacement,
suspension, ambience preemption/restart, concurrent producers, enqueue during
retirement, and hook/resource teardown with another PCM writer. The S3 firmware
build and existing LAN voice/source checks also pass.

On hardware, compare free internal heap, largest internal block and minimum
free heap before playback, during each sound, and after stop/preemption plus
idle-task cleanup. Check repeated start/stop/kind changes, Pomodoro transitions,
preview volume changes, alarm dismissal, AirPlay/A2DP/radio/Talk/voice-note
preemption, ducking/restoration and OTA suspension. Verify stack high-water
marks and absence of clicks/underruns. Initial board observations follow;
full playback and reclamation coverage still needs these checks.


### Initial board observations — 2026-10-21

With user authorization, flashed the S3 over `/dev/cu.usbmodem1101` using the
normal PlatformIO upload; flash hashes verified. No full erase was performed.
The existing partition layout matched, and Wi-Fi and saved storage returned.
The previous app hash began `8cd7bd3f5`; the new app hash begins `990bdccde`.

At the matching AirPlay startup log checkpoint, before background online work:

| Internal heap reading | Previous firmware | Dynamic sound workers |
| --- | ---: | ---: |
| Free bytes | 22,479 | 32,019 |
| Largest free block | 14,336 | 23,552 |

This is an observed **9,540-byte (9.32 KiB)** free-internal increase at that
checkpoint, close to the payload estimate. It is one before/after observation,
not a distribution or a minimum-heap guarantee. Later settled API samples
reported **33 KiB free internal RAM**. API readings have whole-KiB precision
and vary with TLS, networking and concurrent playback.

On-board API smoke checks exercised rain playback, switching to forest, normal
stop/fade-out, 20 rapid play/stop pairs, and two standalone alarm previews at the
existing saved volume. After the fade-out, command burst and each standalone
preview, the API returned to 33 KiB internal / 6,502 KiB PSRAM free. No panic,
assertion, stack overflow or heap-corruption signature appeared in the captured
serial log during this run. Settings values were retained.

The combined ambience/preview run reported 23 KiB internal / 6,495 KiB PSRAM
free, including the final stop sample. The user began manual radio testing
during these checks, so combined playback reclamation and radio preemption
need an isolated follow-up; those samples do not establish recovery or a leak.
Automatic playback commands were stopped when the user confirmed their manual
test. Audible quality, radio connection/playback and remaining source changes
are still under manual verification.

### Playback follow-up

The user reported continuous crackling/distortion on radio while AirPlay was
clean. A host regression then reproduced a control starvation bug: if the
reported DMA queue depth remains below the top-up watermark, the system-sound
worker's unbounded silence loop never returns to command/completion handling.
The regression covers both preview completion and queued cancellation of a
repeating alarm with the cursor held low. The top-up pass is now bounded to
four DMA buffers (enough to cover the 16 ms watermark from empty, including
rounding), returning to control handling and the existing 4 ms task period.
This fixes the reproduced lifecycle defect; its contribution to the reported
radio distortion still requires on-board confirmation.

After flashing the bounded-top-up fix, three isolated on-board combined-sound
cycles each reported 33 KiB internal / 6,502 KiB PSRAM at idle, 27 / 6,499 KiB
with ambience, 23 / 6,495 KiB during the preview, 27 / 6,499 KiB after preview
completion, and 33 / 6,502 KiB after ambience fade-out. Radio remained stopped
and AirPlay inactive throughout these checks. Serial logs confirmed system-sound
worker retirement and ambience stop on every cycle, without panic/assertion/
stack-overflow/heap-corruption signatures. Radio listening confirmation is
pending separately; successful heap recovery alone does not prove audio quality.

At the user's request, a subsequent built-in-microphone check captured raw
16 kHz mono audio at idle, during rain, and during MP3/AAC radio. The MP3
station preempted ambience; both stations reached `playing`. Playback and
microphone teardown again returned to 33 KiB internal / 6,502 KiB PSRAM free.
No microphone ADC clipping was measured, but MP3 acoustic pickup was only
2.62 dB above the idle baseline (AAC 10.14 dB). These captures cannot certify
clean radio output or exclude upstream/speaker distortion at that noise floor.


A further controlled check served a known 997 Hz, approximately -18 dBFS-peak
signal from the Mac as local HTTP radio (MP3 44.1 kHz; AAC 44.1 and 48 kHz).
The nearby Mac microphone recorded it, including a continuous MP3/AAC/MP3
comparison and a quiet 44.1/48 kHz comparison after keyboard noise was identified.
No defined tone-envelope collapse (below 25% of median for >=10 ms) or microphone
ADC clipping was detected in the steady windows. Acoustic harmonic estimates
were low but varied between passes. One quiet 48 kHz recording included a brief
additional nonharmonic component near 3.956 kHz, absent in an earlier 48 kHz pass;
its origin remains unassigned. These measurements do not establish resampling
as the reported crackle's cause or certify all internet-radio playback. The
temporary presets/server were removed; the original preset list and stopped
selection were restored, with 33 KiB internal RAM free. Radio continues using
the existing 44.1 kHz I2S path and linear resampler; no rate-switching change
was made during diagnosis.

All five ambience profiles were subsequently compared against eight-seed renders
of the actual firmware synth at the saved 55% volume, using a quiet continuous
Mac-microphone capture. Brown-versus-pink relative spectral tilt was -2.15 dB/oct
measured versus -2.16 in the references. Using pink as an acoustic-response
calibration anchor, the other profiles' spectral-shape residual RMS was
0.35–0.76 dB across usable third-octave bands. Ocean's swells were consistent;
forest bird-call detail was too faint for reliable verification. Stationary
recorded envelopes varied more than the unfiltered digital references, so the
comparison does not rule out short clicks or isolate the acoustic chain's
contribution. No microphone sample clipped or complete 100 ms block collapsed
below 25% of the profile median. Playback stopped with 33 KiB internal free;
settings were unchanged and no new firmware was flashed for this comparison.
