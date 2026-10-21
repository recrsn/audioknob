# Internal RAM and PSRAM inventory

2026-10-21. Rebased onto main `d110ad4`; sound/location checkpoint `54aca98`.
Includes checkpoint `d7f679a` (reusable network worker and PSRAM artwork stack),
plus the interrupt-driven LVGL encoder change. Both firmware builds passed; neither was flashed for this
inventory. CSV rows include source locations; JSON captures ELF sections and
hashes. All sizes below are reservations/payloads, not measured stack use.

## Normal service stack totals

| Chip / scenario | Known internal stacks | PSRAM stacks |
| --- | ---: | ---: |
| S3, joined network, AirPlay ready | **66.75 KiB + Wi-Fi stack** | **108 KiB** |
| ESP32, Bluetooth enabled | **44.75 KiB** | **0** |

The S3 subtotal includes 49 KiB of application internal stacks and 17.75 KiB of
known framework internal stacks; 104 KiB of application PSRAM stacks plus 4 KiB
for mDNS. Session/setup/install stacks below are additional only when applicable.
The vendor Wi-Fi stack is explicitly excluded from the known internal subtotal:
its linked getter selects 3/6 KiB with an optional 512-byte increment, but the
runtime-selected branch was not measured. Closed controller/driver internals
could own further private allocations, so these are not live task-census totals.

Task control blocks are internal even when their stacks use PSRAM. The S3 build
has 340-byte TCBs; the retained network stack and TCB are already in static
internal data/BSS. Do not add those again when combining ELF and heap totals.
No stack reductions are justified by reservations alone; high-water marks must
be captured under actual TLS, decoding, recording and update workloads.

## S3 stacks

| Task | KiB each | Memory | Instances | Lifetime / allocation |
| --- | ---: | --- | ---: | --- |
| `src_watch` | 3 | internal | 1 | service lifetime after initialization; created once; retained |
| `uart_rx` | 6 | internal | 1 | service lifetime after initialization; created once; retained |
| `airplay_play` | 4 | internal | 1 | service lifetime after initialization; created once; retained |
| `airplay_ctrl` | 6 | internal | 1 | service lifetime after initialization; created once; retained |
| `airplay_art` | 6 | PSRAM | 1 | service lifetime after initialization; created once; retained |
| `messages` | 6 | internal | 1 | service lifetime after initialization; created once; retained |
| `note_store` | 6 | internal | 1 | service lifetime after initialization; created once; retained |
| `ptp` | 4 | internal | 1 | service lifetime after initialization; created once; retained |
| `network_jobs` | 8 | internal | 1 | reserved at boot; task starts on first due job; static stack + static TCB; reused |
| `httpd (settings)` | 6 | internal | 1 | normal joined-network mode; created once; retained |
| `lvgl` | 8 | PSRAM | 1 | service lifetime after initialization; created once; retained |
| `talk` | 40 | PSRAM | 1 | service lifetime after initialization; created once; retained |
| `voice_notes` | 40 | PSRAM | 1 | service lifetime after initialization; created once; retained |
| `talk_discovery` | 4 | PSRAM | 1 | service lifetime after initialization; created once; retained |
| `talk_rtsp` | 3 | PSRAM | 1 | service lifetime after initialization; created once; retained |
| `ap2_listen` | 3 | PSRAM | 1 | service lifetime after initialization; created once; retained |
| `focus_sound` | 4 | internal | 1 | active ambience until fade/preemption; dynamic; freed at task exit |
| `system_sound` | 3 | internal | 1 | alarm/preview until completion or stop; dynamic; freed at task exit |
| `dacp_search` | 4 | internal | 1 | remote discovery, temporary; dynamic; freed at task exit |
| `ap2_rtp` | 4 | internal | 1 | realtime AirPlay stream; dynamic; freed at task exit |
| `ap2_buf_rx` | 3 | internal | 1 | buffered AirPlay stream; dynamic; freed at task exit |
| `ap2_buf_dec` | 8 | PSRAM | 1 | buffered AirPlay stream; dynamic; freed at task exit |
| `ap2_rtsp0..3` | 8 | PSRAM | 4 | RTSP connections, up to four; dynamic; freed at task exit |
| `ap2_event` | 4 | PSRAM | 4 | per connection with event channel; dynamic; freed at task exit |
| `radio_net` | 8 | PSRAM | 1 | radio session; dynamic; freed at task exit |
| `radio_dec` | 8 | PSRAM | 1 | radio session; dynamic; freed at task exit |
| `talk_conn` | 6 | internal | 2 | incoming connection, up to two; dynamic; freed at task exit |
| `wifi_setup` | 4 | internal | 1 | setup mode; dynamic; freed at task exit |
| `httpd (portal)` | 6 | internal | 1 | setup mode, replaces settings HTTP server; dynamic; freed at task exit |
| `dns_portal` | 3 | internal | 1 | setup mode; dynamic; freed at task exit |
| `wifi_forget` | 3 | internal | 1 | forget credentials then reboot; dynamic; freed at task exit |
| `s3_fw OR esp32_fw` | 4 | internal | 1 | install after physical confirmation; dynamic; freed at task exit |
| `IDLE0 / IDLE1` | 1.5 | internal | 2 | system lifetime; created once; retained |
| `ipc0 / ipc1` | 1.25 | internal | 2 | system lifetime; created once; retained |
| `esp_timer` | 4 | internal | 1 | system lifetime; created once; retained |
| `Tmr Svc` | 2 | internal | 1 | system lifetime; created once; retained |
| `main` | 4 | internal | 1 | boot until app_main returns; ESP32 BT-off sleep retains it; released after app_main returns |
| `sys_evt` | 2.75 | internal | 1 | network service lifetime; created once; retained |
| `tiT / TCP-IP` | 3.5 | internal | 1 | network service lifetime; created once; retained |
| `mdns` | 4 | PSRAM | 1 | mDNS service lifetime; retained PSRAM stack; static internal TCB |
| `wifi / ppTask` | vendor-selected | internal | 1 | Wi-Fi enabled; vendor library selects size |

## ESP32 stacks

| Task | KiB each | Memory | Instances | Lifetime / allocation |
| --- | ---: | --- | ---: | --- |
| `IDLE0 / IDLE1` | 1.5 | internal | 2 | system lifetime; created once; retained |
| `ipc0 / ipc1` | 1 | internal | 2 | system lifetime; created once; retained |
| `esp_timer` | 4 | internal | 1 | system lifetime; created once; retained |
| `Tmr Svc` | 2 | internal | 1 | system lifetime; created once; retained |
| `main` | 4 | internal | 1 | boot until app_main returns; ESP32 BT-off sleep retains it; released after app_main returns |
| `state_sync` | 3 | internal | 1 | BT enabled; state_sync/cmd_rx also run with BT disabled; created once; retained |
| `cmd_rx` | 6 | internal | 1 | BT enabled; state_sync/cmd_rx also run with BT disabled; created once; retained |
| `encoder` | 3 | internal | 1 | BT enabled; state_sync/cmd_rx also run with BT disabled; created once; retained |
| `i2s_out` | 3 | internal | 1 | BT enabled; state_sync/cmd_rx also run with BT disabled; created once; retained |
| `bt_app_task` | 4 | internal | 1 | BT enabled; state_sync/cmd_rx also run with BT disabled; created once; retained |
| `btdm controller` | 4 | internal | 1 | Bluetooth enabled; created once; retained |
| `BTC_TASK` | 3.5 | internal | 1 | Bluetooth enabled; created once; retained |
| `BTU_TASK` | 4.75 | internal | 1 | Bluetooth enabled; created once; retained |
| `hciT` | 2.5 | internal | 1 | Bluetooth enabled; created once; retained |

## Interpretation and inactive task templates

- The S3 shared `network_jobs` stack is reserved statically at boot, starts once,
  and blocks between jobs. Weather, location/NVS, release discovery and reserved
  OTA downloads reuse it. There is no separate `online`, `release_check` or
  `ota_download` task in this working tree.
- `focus_sound` and `system_sound` can run simultaneously. Their stacks cannot
  share one live backing buffer. Independent retained stacks would be possible,
  but this inventory does not change the audio workers.
- AirPlay buffered and realtime receive paths are conditional, not one additive
  always-on total. RTSP/event and Talk connection rows show per-instance sizes
  and limits; they do not assert all instances occur together. Task deletion
  can temporarily leave retired stack allocations pending idle-task cleanup.
- ESP32 Bluetooth-off boot starts only the UART/state-sync application tasks
  and sleeps on its 4 KiB main stack; it skips Bluetooth, encoder and I2S workers.
  Its application stacks are therefore 13 KiB in that mode, plus system tasks.
- ESP32 does not initialize Wi-Fi, TCP/IP or the default event loop in its normal
  Bluetooth app. Configured TCP/IP/event stack defaults are not reservations.
- LVGL runs with `LV_OS_NONE`: its configured 8 KiB draw-thread template does
  not create another task. S3 mDNS socket receive mode is disabled, so the
  optional 3 KiB `mdns recv task` is absent. pthread default stack size and
  SmartConfig/BLE logging templates do not imply live tasks either.
- Framework `main`, esp_timer, event, TCP/IP and Bluetooth host/controller sizes
  include the applicable 512-byte full-newlib allowance from `esp_task.h`.
  Idle stacks use their separate FreeRTOS overhead calculation (zero extra in
  these configs); FreeRTOS timer and IPC sizes do not get that allowance.

## Other stack reservations

| Reservation | Internal bytes | Notes |
| --- | ---: | --- |
| S3 interrupt stacks, both cores | 4,192 | `port_IntStack` in ELF; 2 × 2,096 |
| S3 core-dump stack allocation | 1,892 | ELF symbol; 1,792 configured plus implementation bookkeeping |
| ESP32 interrupt stacks, both cores | 3,072 | `port_IntStack`; 2 × 1,536 |

These are already in the linked internal data/BSS totals, not extra heap tasks.

## Linked internal storage

| Chip | Internal data + BSS | Internal-resident executable/vector storage | Static PSRAM data/BSS |
| --- | ---: | ---: | ---: |
| S3 | 77,092 bytes (75.29 KiB) | 121,623 bytes (118.77 KiB) | 0 |
| ESP32 | 54,724 bytes (53.44 KiB) | 103,072 bytes (100.66 KiB) | 0 |

The data/BSS figures include global state, static stacks, interrupt stacks and
static synchronization storage, but exclude dynamically created stacks, driver
allocations, queues, most buffers and protocol heaps. RTC allocations are separate.
The `.dram0.dummy`, `.ext_ram.dummy` and flash-rodata mapping holes are address
layout reservations, not additional allocated buffers. Flash-resident assets
are not PSRAM until copied. These ELF totals cannot establish free internal RAM,
PSRAM usage, heap fragmentation or a contiguous-block guarantee.

## Major non-stack buffers

| Allocation | Payload size | Placement / lifetime |
| --- | ---: | --- |
| LCD draw buffer | **43,200 B / 42.19 KiB** | Internal DMA, permanent; 360 × 60 × RGB565 |
| S3 I2S output DMA data | 5,760 B | Internal DMA, 6 × 240 stereo 16-bit frames; descriptors/control extra |
| S3 microphone DMA data | 5,120 B | Internal DMA while mic open; 160 ms mono 16-bit; descriptors/control extra |
| S3 I2S mixing scratch | 960 B | Static internal BSS, already counted above |
| OTA receive chunk | 4,096 B | Explicit internal heap while receiving; shared worker stack is separate |
| S3 UART driver RX/TX | 4,096 + 256 B | Internal driver buffers; driver metadata extra |
| LVGL TLSF pool | **384 KiB** | Explicit PSRAM; LVGL objects/scratch allocate within this pool, so do not double-count them |
| Clock and wireless full-screen backgrounds | 259,200 B each | Ordinary malloc, PSRAM-preferred for these large allocations; two built screens ≈ 506.25 KiB; allocator fallback possible |
| AirPlay PCM slots | **569,600 B / 556.25 KiB** | Explicit PSRAM, permanent; 400 × `sizeof(slot_t)==1424` |
| UART artwork staging | 128 KiB | Explicit PSRAM, permanent |
| Artwork double buffer | 69,696 B | Explicit PSRAM; 2 × 132 × 132 × RGB565 |
| Radio preset list | 7,296 B | Explicit PSRAM; 24 × `sizeof(radio_station_t)==304` |
| Inbox/outbox stores | 3,680 B | Explicit PSRAM; 2 × `sizeof(store_t)==1840`; snapshots/JSON additional |
| AirPlay buffered compressed ring | 256 KiB–1 MiB + 1 B | Explicit PSRAM while buffered stream exists, tries 1 MiB first |
| AirPlay buffered decode scratch | 36,871 B | Explicit PSRAM; 2 × 16 KiB blocks, 7-byte AAC headroom, 4 KiB PCM; codec state extra |
| AirPlay buffered control object | 412 B | Explicit internal heap while buffered stream exists |
| AirPlay realtime reorder window | 92,160 B / 90 KiB | Explicit PSRAM; 64 × `sizeof(held_t)==1440` |
| AirPlay realtime scratch + state | 8,616 B | PSRAM: 4 KiB PCM, two 2 KiB packet/plain buffers, 424-byte state; codec state extra |
| Radio compressed ring | **512 KiB** | Explicit PSRAM during radio session; ring control extra |
| Radio decoder scratch/state | 18,504 B + variable output | Explicit PSRAM; codec allocations prefer PSRAM with eligible-heap fallback |
| Talk session | 15,256 B + codec allocations | Explicit PSRAM while session exists |
| Recorded voice-note staging | up to 300 KiB | Explicit PSRAM during record/save; loaded/read note buffers are additional, length-dependent |
| Ambience rendering | 3,072 B | PSRAM during ambience; synth ordinary malloc is another 1,948 B (internal-preferred) |
| System-sound lookup table | 4,100 B | Explicit PSRAM while system sound needs it |
| Weather/location response | 8 KiB per job | Explicit PSRAM, freed after each job |
| Release discovery response | 64 KiB | Explicit PSRAM during discovery; parsed JSON nodes also PSRAM |
| ESP32 I2S ring | 32 KiB | Internal heap; ESP32 has no PSRAM |
| ESP32 UART driver RX/TX | 1 KiB + 16 KiB | Internal driver buffers; metadata extra |
| ESP32 Bluetooth controller reserved DRAM | 56,156 B configured | Separate controller reservation; BLE-only portions released at boot; not a measured active-heap charge |

Other variable allocations include TLS/HTTP connections, lwIP packet/socket
buffers, Wi-Fi/BT pools, codec state, microphone noise suppression, artwork
parsing, LVGL temporary layers, queues, mutexes and file-system caches. They
cannot be assigned one truthful fixed total from this ELF. Ordinary allocations
up to 16 KiB prefer internal memory on S3; larger allocations prefer PSRAM,
with eligible-heap fallback. Wi-Fi/lwIP are allowed to use PSRAM where supported.
The 32 KiB internal allocator reserve is a policy, not another allocated buffer.

## Reuse and validation

The shared network stack/TCB, LVGL stack, voice workers and other persistent
service stacks are reused. The remaining stack churn comes from session audio,
connection/setup tasks, sound workers, discovery and OTA installation. A generic
cross-service stack pool would need concurrency and internal-vs-PSRAM constraints;
several of these services can overlap, so summing their sizes does not identify
safe sharing automatically. This report makes no stack/pool changes beyond the
previously authorized shared worker and its necessary post-rebase OTA integration.

Next board measurements are a full task census (stack bounds and high-water
marks), per-capability current/minimum free heap and largest block, and repeated
radio/AirPlay/voice/TLS/OTA transitions. The current board still runs earlier
firmware. No flash, new implementation commit or push was performed for this
inventory. Heap and high-water measurements are explicitly pending.


## Subsequent authorized flash

After this static inventory, the S3 was flashed on 2026-10-21 and returned online
as `v0.1.6-1-g54aca98-dirty`. Saved location/timezone loaded, weather/AQI populated,
and startup release discovery finished successfully. A settled API sample showed
48 KiB free internal / 6,268 KiB free PSRAM. This is not a stack high-water or
fragmentation measurement; see the appended board note in `memory-budget.md`.


## Artwork stack placement update

`airplay_art` now retains its 6 KiB stack in PSRAM. Priority 1, no core affinity,
stack depth and artwork queue/decoding behavior are unchanged; its task control
block stays internal. The path decodes memory-backed JPEG/PNG, publishes pixels
under a mutex and frees scratch buffers; it does not access NVS, partitions or
files. This moves 6 KiB of stack reservation from internal RAM to PSRAM without
adding recurring stack allocations. The 48 KiB board sample above predates this
change; this artwork-stack update has not been flashed or measured on hardware.

## Encoder task removed

The hardware knob now captures timestamped GPIO edges into a fixed 64-entry
internal ring (512 bytes) and decodes them through an LVGL encoder device. UI
callbacks run on the existing LVGL PSRAM stack. Its dedicated 3 KiB internal
stack and TCB are gone; the ring and decoder state add static internal storage,
so 3 KiB is the stack saving, not the total net RAM saving. No high-water-based
stack trimming was performed. Physical encoder validation remains outstanding.
