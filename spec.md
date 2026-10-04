# ESP32-S3-Knob-Touch-LCD-1.8 — Desk Companion Audio Streamer
## Final Requirements
Hardware: Waveshare ESP32-S3-Knob-Touch-LCD-1.8 (dual MCU: ESP32-S3R8 + ESP32-U4WDH, PCM5100A DAC, CH445P I2S switch, CST816 touch, dual rotary encoder, DRV2605 haptics, PDM mic, microSD)
All hardware facts below are confirmed against the actual schematic sheets (LCD_POWER, ESP32S3-R8, ESP32-CHIP, OTHER, DAC), not vendor marketing copy.
---
## 1. System Architecture
**Single active audio source at a time.** If a new source connects while another is active, the old source is disconnected/muted and the new one takes over. No mixing, no simultaneous playback.
| Mode | Owner MCU | Path |
|---|---|---|
| WiFi / AirPlay | ESP32-S3 | WiFi → RAOP/ALAC decode → I2S (GPIO39/40/41) → CH445P → PCM5100A |
| Classic BT / A2DP | ESP32-U4WDH | Classic BT → A2DP sink → I2S (IO25/26/27) → CH445P → PCM5100A |
| USB-C UAC | ESP32-S3 | TinyUSB UAC2 → I2S (GPIO39/40/41) → CH445P → PCM5100A |
AirPlay and USB share the S3's I2S output path and CH445P selection state (both are "S3 side" from the switch's perspective) — switching between them is an S3-internal software decision, not a CH445P reselection. A2DP is the only ESP32-side source.

**Why AirPlay lives on the S3, not the ESP32**: the ESP32-U4WDH has no PSRAM — only its 520 KB of internal SRAM, most of which Bluedroid already uses for A2DP. AirPlay 1 needs a ~2 s jitter/sync buffer (~0.5 MB of decoded PCM), and every existing ESP32 AirPlay receiver (e.g. squeezelite-esp32) requires 4 MB of PSRAM for exactly that reason. The S3 has 8 MB of PSRAM and 16 MB of flash. Splitting the radios this way (WiFi on the S3, Classic BT on the ESP32) also removes single-radio WiFi/BT time-slicing, a known source of A2DP dropouts. The two radios are on separate chips with separate antennas and no coexistence signalling between them; in practice only one source streams at a time, and an idle WiFi association carries little traffic.

**AirPlay scope**: AirPlay 1 (RAOP) only — ALAC 44.1 kHz/16-bit/stereo, RSA/AES-encrypted or clear, text metadata (title/artist/album) via DMAP, DACP back-channel for volume and play/pause. AirPlay 2 needs HomeKit pairing and PTP timing and is out of reach on this hardware; iOS still streams to AirPlay 1 receivers.
**Source switch**: ESP32-S3 GPIO0 drives CH445P select (`I2S_SWITCH_IN`): high = S3 side, low = ESP32 side (see §7). The S3 has firmware-level authority over which MCU's I2S reaches the DAC, regardless of which MCU owns local business logic for that mode (see Control Ownership below). *Caveat: GPIO0 is a boot-strap pin — do not drive it until app init is complete, past power-on bootloader sampling.*
**Inter-MCU link**: dedicated UART, S3 GPIO38(TX)/GPIO48(RX) ↔ ESP32 IO18(TX)/IO23(RX). Full duplex, confirmed on both schematic sheets. Carries control/metadata only (now-playing info, volume state, connection status, mode-switch requests, touch-originated commands). Not used for audio — I2S/CH445P handles that with zero CPU cost and no UART bandwidth concern.
**Control ownership rule**: whichever MCU owns the currently active audio path also owns local input (its own encoder) and volume state for that path. The other MCU is a pure display + touch-passthrough peripheral for that mode.
- **A2DP**: ESP32 is authoritative. It reads its own encoder (`EC2_A`/`EC2_B`) directly, applies volume locally, translates to/from A2DP/AVRCP absolute-volume messages, and pushes state (title/artist, volume level, connection status) to the S3 over UART for display only. The S3's own encoder is inactive in this mode; touch input on the S3 (e.g. play/pause tap) is forwarded to the ESP32 as a command, not executed locally.
- **AirPlay**: S3 is authoritative. It reads its own encoder (`EC1_A`/`EC1_B`) and `SW2`, applies volume as software gain on its own PCM, pushes knob changes to the sender over DACP (`dmcp.device-volume`), applies the sender's slider (RTSP `SET_PARAMETER volume`) locally, and sends play/pause over DACP. The S3 knob drives AirPlay while AirPlay holds the DAC, or has a session open while A2DP is idle; otherwise the ESP32's knob owns wireless volume.
- **USB mode**: S3 is authoritative. It owns the UAC2 Feature Unit, reads its own encoder (`EC1_A`/`EC1_B`) directly, and syncs volume bidirectionally with the host OS (see Mode B below). The ESP32 plays no role in this mode.
- Rationale: each encoder is physically wired to the MCU that owns its mode's protocol stack, so this avoids round-tripping every knob tick through the inter-MCU UART, and keeps protocol-specific logic (AirPlay/AVRCP framing, UAC Feature Unit semantics) local to the chip that already has to implement that protocol anyway.
**Toolchain**: ESP-IDF, not Arduino. Required because Arduino-ESP32's TinyUSB build doesn't compile in `CFG_TUD_AUDIO` — USB Audio Class is unavailable there regardless of framing.
### Shared Resource Arbitration
Only two resources in the system need an explicit cross-chip ownership rule. Audio/DAC is not one of them — the CH445P resolves that at the hardware level (a clean mux, not a software policy). Everything else (LCD, touch, haptics, battery ADC, mic, SD) is permanently single-owner hardware with no ambiguity.
- **Encoder**: not hardware-shared — each MCU has its own physical quadrature GPIOs (`EC1_A/B` on S3, `EC2_A/B` on ESP32). The only thing needing a rule is *whose reading is authoritative*, governed by the Control Ownership rule above (whichever MCU owns the active mode owns its own encoder's input).
- **WiFi**: both MCUs have their own onboard WiFi radio and antenna (confirmed on both schematic sheets — S3's L1/L2/ANT1, ESP32's L5/L6/ANT2), but **only the S3 ever brings its WiFi up in this design** (it hosts AirPlay — see §1). The ESP32's WiFi radio stays dormant; its radio is Classic BT only. This avoids the device presenting two independent network identities, and avoids two WiFi radios a few centimeters apart transmitting simultaneously. The S3 runs its own SNTP client for the clock.
---
## 2. Inter-MCU State Sync Protocol
Governs how A2DP state (volume, mute, connection, playback, track) reaches the S3's display over the inter-MCU UART. AirPlay state never crosses the UART — it originates on the S3. (An earlier NTP relay frame, type 0x02, was retired when NTP moved to the S3.)

The reverse direction carries commands: `MSG_COMMAND` (0x04, S3→ESP32) with a one-byte `cmd` payload, same framing. Fire-and-forget — no ACK; the ESP32's next `STATE_UPDATE` shows the effect. Defined commands: `CMD_PAUSE` (0x01, stop A2DP playback). Touch-originated A2DP commands should extend this enum rather than add frame types.
### Framing
Fixed format, no escaping needed — this is a short on-board trace, not a noisy long cable run:
```
[SYNC 0x7E][TYPE][LEN][PAYLOAD...][CRC8]
```
### Payload — single combined state struct
One `STATE_UPDATE` type carries everything at once rather than separate volume/connection/playback messages:
```c
typedef struct {
    uint8_t volume;     // 0–100
    uint8_t muted;       // bool
    uint8_t connected;   // bool
    uint8_t playing;     // bool
    uint8_t source;      // 0=AirPlay, 1=A2DP
} wireless_state_t;
```
5 bytes payload, 9 bytes total on the wire including framing.
### Rate limiting at the source (ESP32)
Never send a frame per encoder edge — decouple encoder tick rate from UART/display update rate:
1. Encoder ISR/task updates a local `current_volume` variable as fast as it wants — no UART involved, no cost
2. A periodic tick task (~33ms, matching LVGL's typical redraw rate — no benefit going faster than the screen can show) compares the current state against what was last sent
3. If anything changed, send one `STATE_UPDATE` frame with the current snapshot; if nothing changed, skip
A fast knob spin collapses into "send the latest position every 33ms" rather than one message per detent — less UART traffic and less LVGL redraw churn, with no perceptual loss.
### Resync safety
Resend the full state every ~1s regardless of whether it changed (same frame type, just on a slower unconditional timer). This gives self-healing after a dropped/corrupted frame within about a second, without needing sequence numbers or an ACK/retry protocol — appropriate given how short and low-traffic this link is.
### S3-side handling
The UART receive task parses frames and writes the latest `wireless_state_t` into a shared struct (mutex or single-writer/single-reader queue). The LVGL task reads that struct on its own tick to update the volume bar, mute icon, and connection indicator. Neither task blocks on the other — UART reception never stalls behind a slow LVGL redraw, and LVGL never blocks waiting on UART.
---
## 3. Functional Modes
### Mode A — Wireless (WiFi/AirPlay + Classic BT/A2DP)
Both sub-sources are always available; there is no manual sub-source toggle. **Last source to start playing wins**: when one starts while the other holds the DAC, the S3 flips the CH445P to the new source and the displaced one is told to stop (AirPlay is paused over DACP, or its session dropped if the sender exposes no DACP remote). When AirPlay displaces A2DP, the S3 sends a `MSG_COMMAND`/`CMD_PAUSE` frame (§2); the ESP32 pauses the phone over AVRCP, or drops the A2DP link if the phone exposes no AVRCP controller. A paused AirPlay session hands the DAC back immediately.

**A2DP (ESP32)**
- ESP32 owns Classic BT + A2DP sink profile — fully authoritative per Control Ownership rule above
- Own encoder (`EC2_A`/`EC2_B`, IO19/IO22) for local volume; AVRCP absolute-volume (where the paired source supports it) stays in sync with local knob position via explicit sync logic, not "last write wins"
- Own push-button (`SW1`) for local play/pause; also usable for mode-switch-request
- S3 is a dumb display peripheral for A2DP: renders whatever state ESP32 sends over UART per the State Sync Protocol (§2) — title/artist/album, volume level, connection status — and forwards S3-side touch taps back to ESP32 as commands. Applies no independent volume logic.
- Drives DAC directly via own I2S bus when CH445P is selected to ESP32 side

**AirPlay (S3)**
- S3 owns WiFi + mDNS (`_raop._tcp`) + RTSP/RAOP session handling + AES decrypt + ALAC decode — fully authoritative per Control Ownership rule above
- Playback is scheduled to the sender's clock: decoded PCM (~2 s ahead, held in PSRAM) is released at its RTP/NTP-derived playtime, with small skip/pad corrections keeping the DAC from drifting
- Own encoder (`EC1_A`/`EC1_B`) for volume, kept in sync with the sender's slider via DACP; `SW2` for play/pause via DACP
- Drives DAC directly via own I2S bus when CH445P is selected to S3 side
### Mode B — USB-C UAC (device)
- S3 owns TinyUSB UAC2 stack, appears as a USB sound card to host PC — fully authoritative per Control Ownership rule above
- **Volume**: bidirectional sync through the UAC2 Feature Unit, single source of truth on the S3.
  - Encoder rotation (`EC1_A`/`EC1_B`, GPIO07/08) → firmware updates the Feature Unit's current volume value → firmware sends the interrupt/status notification so the host OS's slider reflects the change
  - Host-side slider change → firmware receives the `SET_CUR` control request → updates applied gain and the on-screen LVGL indicator
  - No double-attenuation: exactly one point applies gain, both UIs (knob and OS slider) just read/write the same register
- Own push-button (`SW2`) for mode-switch-request
- Drives DAC directly via own I2S bus when CH445P is selected to S3 side
- No track metadata available from USB audio class alone — Now Playing screen in this mode shows spectrum + volume/mute state, not title/artist
### Mode C — Timer / Clock
- Runs entirely on S3 (no networking or audio-source dependency)
- Reuses same knob + touch + haptic input pattern as A/B
- **Clock**: NTP-synced time display, timezone set via knob. The S3 runs SNTP over its own WiFi (see Shared Resource Arbitration); clock free-runs off RTC between syncs
- **Timer/Pomodoro**: knob sets duration, tap to start, haptic buzz (DRV2605) on completion
- Selectable from the same mode-switch UI as A/B; does not require CH445P switch state change
---
## 4. UI (LVGL, all screens on S3)
- **Mode select** (top-level swipe/touch menu): Wireless / USB / Timer & Clock
- **Now Playing (Wireless)**: cover art, title/artist, spectrum analyzer (FFT tapped directly from the live I2S PCM buffer, not re-captured acoustically via the mic), volume indicator (state pushed from ESP32), connection status
- **Now Playing (USB)**: spectrum analyzer (same direct-tap method), volume indicator (state owned locally by S3), "connected/idle" state — no metadata field
- **Timer/Clock**: time display, knob-set countdown, start/pause/reset via touch or button
---
## 5. Peripheral Integration
| Peripheral | Confirmed net/component | Use |
|---|---|---|
| DRV2605 haptic driver | I2C (`HAPTIC_SCL/SDA/TRIG/EN`, shares S3's touch I2C bus) | Mode-switch confirm tap, connect/disconnect event, timer-complete buzz |
| Battery ADC | `BATT_ADC` → ESP32-S3 GPIO01, resistor divider (R62/R63, 10K/10K) | Battery icon in status bar (relevant mainly for untethered wireless-only use — USB mode implies wired anyway) |
| PDM mic | MSM261D4030H1CPM | **Not used** — spectrum analyzer sources from the live audio buffer instead |
| microSD | SDMMC bus | Reserved, not required for MVP |
---
## 6. Non-Functional Notes
- Audio buffering: size I2S/UAC buffers to avoid underrun clicks — a scheduling/latency concern, not a compute-bandwidth one
- Mode switching between USB and wireless should cleanly tear down the previous mode's stack before initializing the next, rather than attempting concurrent operation. Within wireless, AirPlay (S3 WiFi) and A2DP (ESP32 BT) stay up concurrently by design — they're on separate chips — and are arbitrated at the CH445P (§3 Mode A)
- Memory: 16MB flash / 8MB PSRAM on S3 is comfortably above every referenced project's stated minimum
- Inter-MCU UART: standard baud (115200+) is comfortably sufficient for control/metadata traffic (~11.5 KB/s effective at 115200); never intended to carry audio (stereo 16-bit 44.1kHz PCM alone needs ~1.4 Mbps, which is what the dedicated I2S/CH445P path is for)
---
## 7. Pin / Net Reference
### ESP32-S3R8 (U2) — confirmed nets
| GPIO | Net / Function |
|---|---|
| GPIO00 | `I2S_SWITCH_IN` — drives CH445P audio-source select. **Boot-strap pin — do not drive before app init.** |
| GPIO01 | `BATT_ADC` |
| GPIO02 | `SDMMC_D3` |
| GPIO03 | `SDMMC_CMD` |
| GPIO04 | `SDMMC_SCK` |
| GPIO05 | `SDMMC_D0` |
| GPIO06 | `SDMMC_D1` |
| GPIO07 | `EC1_B` (S3-side encoder) |
| GPIO08 | `EC1_A` (S3-side encoder) |
| GPIO09 | `TP_INT` |
| GPIO10 | `TP_RST` |
| GPIO11 | `TP_SDA` (shared with `HAPTIC_SDA`) |
| GPIO12 | `TP_SCL` (shared with `HAPTIC_SCL`) |
| GPIO13 | `LCD_QSPI_SCL` |
| GPIO14 | `LCD_QSPI_CS` |
| GPIO15 | `LCD_QSPI_D0` |
| GPIO16 | `LCD_QSPI_D1` |
| GPIO17 | `LCD_QSPI_D2` |
| GPIO18 | `LCD_QSPI_D3` |
| GPIO19 | `USB_DN` — native USB D− |
| GPIO20 | `USB_DP` — native USB D+ |
| GPIO21 | `LCD_RST` |
| GPIO38 | `ESP32S3_TX` — inter-MCU UART |
| GPIO39 | `S3_I2S_DAC_BCK` |
| GPIO40 | `S3_I2S_DAC_LRCK/WS` |
| GPIO41 | `S3_I2S_DAC_DIN` |
| GPIO42 | `SDMMC_D2` |
| GPIO45 | `PDM_MIC_SCK` |
| GPIO46 | `PDM_MIC_DATA` |
| GPIO47 | `LCD_BLK` (backlight, via Q1 A03400A MOSFET) |
| GPIO48 | `ESP32S3_RX` — inter-MCU UART |
### ESP32-U4WDH (U14) — confirmed nets
| GPIO (IOxx) | Net / Function |
|---|---|
| IO18 | `ESP32S3_TX` — inter-MCU UART (matches S3's GPIO48 RX) |
| IO19 | `EC2_A` (ESP32-side encoder) |
| IO22 | `EC2_B` (ESP32-side encoder) |
| IO23 | `ESP32S3_RX` — inter-MCU UART (matches S3's GPIO38 TX) |
| IO25 | `ESP32_I2S_DAC_BCK` |
| IO26 | `ESP32_I2S_DAC_DIN` |
| IO27 | `ESP32_I2S_DAC_LRCK/WS` |
| IO32 | `XSMT` (also DAC mute-related) |
| U0TXD/U0RXD | ESP32 UART0 — to host-facing USB-UART bridge (separate from inter-MCU UART) |
### CH445P (U18) — I2S switch
| Pin group | Net |
|---|---|
| S1 bank (in) | `ESP32_I2S_DAC_BCK/DIN/LRCK-WS` (S1A/S1B/S1C, pins 16/3/9) |
| S2 bank (in) | `S3_I2S_DAC_BCK/DIN/LRCK-WS` (S2A/S2B/S2C, pins 1/4/8) |
| D bank (out, shared) | `I2S_DAC_BCK/DIN/LRCK-WS` → PCM5100A |
| Select | `I2S_SWITCH_IN` ← ESP32-S3 GPIO00 |

Per the CH445P datasheet, `EN#`=0 and `IN`=0 selects S1, `IN`=1 selects S2 — so **GPIO0 high routes the S3 to the DAC, low routes the ESP32**. (Waveshare's stock S3 audio demo never drives GPIO0 and relies on its boot-time pull-up to reach the DAC, which agrees.)
### PCM5100A DAC (U12)
| Pin | Net |
|---|---|
| BCK | `I2S_DAC_BCK` (from CH445P output) |
| DIN | `I2S_DAC_DIN` (from CH445P output) |
| LRCK | `I2S_DAC_LRCK/WS` (from CH445P output) |
| OUTL/OUTR | Analog out → 3.5mm jack |
### DRV2605 Haptic Driver (U13)
| Pin | Net |
|---|---|
| SCL | `HAPTIC_SCL` (shared with `TP_SCL`, S3 GPIO12) |
| SDA | `HAPTIC_SDA` (shared with `TP_SDA`, S3 GPIO11) |
| IN/TRIG | `HAPTIC_TRIG` |
| EN | `HAPTIC_EN` |
### Push buttons
| Switch | Wired to | Function |
|---|---|---|
| SW1 | `EC2_A`/`EC2_B` short-to-GND | ESP32-side encoder push (wireless modes) |
| SW2 | `EC1_A`/`EC1_B` short-to-GND | S3-side encoder push (USB mode) |
---
## 8. Reference Links
**Product / schematic**
- Waveshare wiki (product page, demos, resource downloads): https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8
- Schematic (zip): https://files.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8/ESP32-S3-Knob-Touch-LCD-1.8-schematic.zip
- Stock demo code (zip): https://files.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8/ESP32-S3-Knob-Touch-LCD-1.8-Demo.zip
- Stock firmware BIN (zip): https://files.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8/ESP32-S3-Knob-Touch-LCD-1.8-BIN.zip
**Chip datasheets**
- ESP32-S3 Datasheet: https://files.waveshare.com/wiki/common/Esp32-s3_datasheet_en.pdf
- ESP32-S3 Technical Reference Manual: https://files.waveshare.com/wiki/common/Esp32-s3_technical_reference_manual_en.pdf
- ESP32 Datasheet: https://files.waveshare.com/wiki/common/Esp32_datasheet_en.pdf
- ESP32 Technical Reference Manual: https://files.waveshare.com/wiki/common/Esp32_technical_reference_manual_en.pdf
- PCM5100A Datasheet: https://files.waveshare.com/wiki/common/Pcm5100a.pdf
- DRV2605 Datasheet: https://files.waveshare.com/wiki/common/Drv2605.pdf
- CST816 Touch Controller Datasheet: https://files.waveshare.com/wiki/common/CST816D_datasheet_En_V1.3.pdf
**USB Audio Class (UAC) implementation**
- ESP-IoT-Solution USB Device UAC guide: https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_device/usb_device_uac.html
- `esp_device_uac` component example (ESP32-S3 speaker/mic): https://components.espressif.com/components/espressif/usb_device_uac/versions/1.2.2/examples/usb_uac?language=en
- TinyUSB application guide (ESP-IoT-Solution): https://docs.espressif.com/projects/esp-iot-solution/en/latest/usb/usb_overview/tinyusb_guide.html
- Why Arduino-ESP32 can't do USB Audio (context for ESP-IDF requirement): https://github.com/espressif/arduino-esp32/issues/12053
**Community hardware references**
- nkinnan reverse-engineering repo (pinout notes, flashing quirks, both-orientation USB behavior): https://github.com/nkinnan/Waveshare-ESP32-S3-Knob-Touch-LCD-1.8_and_Guition-K5-Knob-Series-JC3636K518
- KrX3D ESPHome config for this board: https://github.com/KrX3D/WaveShare-Knob-Esp32S3
- CNX Software hardware overview: https://www.cnx-software.com/2025/06/25/battery-powered-knob-display-board-pairs-esp32-s3-and-esp32-wireless-socs-features-audio-dac-for-audio-visualization/
**AirPlay/multi-protocol reference (for lessons on S3 porting friction, not for direct reuse given lean-build scope)**
- squeezelite-esp32 ESP32-S3 support discussion: https://github.com/sle118/squeezelite-esp32/discussions/236
