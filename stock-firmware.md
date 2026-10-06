# Waveshare stock firmware: feature catalog

Reverse-engineered from Waveshare's prebuilt images
(`ESP32-S3-Knob-Touch-LCD-1.8-BIN.zip`) by disassembly and string analysis.
No source was available, so this lists what the binaries contain, not
necessarily what is reachable on this board.

| Image | Chip | Build |
|---|---|---|
| `WX-ESP32S3-KNOB_V1.2.bin` | ESP32-S3 | Arduino-ESP32 3.x on IDF 5.1.4, Feb 2025. LVGL 8, ESP32_Display_Panel 0.2.2, `esp_new_jpeg`, `esp_audio_codec` |
| `ESP32-KNOB_ESP32_0.bin` | ESP32-U4WDH | Plain IDF 5.4 project `TAIJI_KNOB_32`, Apr 2025, built on Espressif's `a2dp_sink` example |

The S3 image is one firmware shared across a family of devices by an
independent designer, not by VIEWE (see [Device family](#device-family)). Its
model table lists `TAIJI KNOB 32` (this board), `TAIJI KNOB HUB`,
`TAIJI VIEWE PI`, `YUYING`, `VIEWE HUB` and `JC3636W518C`. Features marked
*(family)* need hardware this board lacks.

## Device family

"TAIJI" is 太极, which is also spelled "Taichi". The firmware is the work of
**萨纳兰的黄昏** ("Dusk of Thanalan"; GitHub/OSHWHub `planevina`; site
[pressf5.run](https://pressf5.run)), a hobbyist designer in Sichuan who sells
his own devices on Xianyu and Bilibili 工房. Guition and Waveshare ship his
firmware on their mass-produced versions of his knob. The evidence:

- The About screen in Waveshare's V1.2 image carries his credit,
  `Design: 萨纳兰的黄昏`.
- The NVS namespace is `GODVISION`, taken from his open-source
  [genshin_godeye_esp32](https://github.com/planevina/genshin_godeye_esp32)
  ("神之眼").
- The image was built from `C:/Users/Fei/.platformio`, against his fork of
  `platform-espressif32`.
- Guition's own knob demo carries a `bidi_switch_knob.h` marked "Modified by
  planevina 2025-01-20".
- His changelog covers exactly `TAIJI_KNOB_32`, `TAIJI_VIEWE_PI` and
  `TAIJI_KNOB_HUB` ([p=186](https://pressf5.run/?p=186)), and his manual
  states which hardware each app needs ([p=119](https://pressf5.run/?p=119)).
  First found via
  [gopro2027's dump notes](https://github.com/gopro2027/ArduinoAirSuspensionController/blob/main/Wireless_Controller/device_libs/ws1p8knob/original_firmware_dump.md).

"VIEWE" in the model names is the romanized 优 of 太极优派, not the VIEWE
display company. VIEWE (viewedisplay.com) does sell ESP32 knob displays
(UEDX46460015 1.5″ AMOLED, UEDX48480021 2.1″, UEDX24240013 1.28″ C3), but they
are single-MCU boards, and nothing links them to this firmware *(the reading of
优 is inference)*.

| Model string | What it is | Who makes and sells it | Hardware | Sources |
|---|---|---|---|---|
| `TAIJI KNOB 32` | 蓝牙音乐旋钮, the "Bluetooth music knob". **This board.** | Designed by 萨纳兰的黄昏. Mass-produced by **Guition** as JC3636K518C ("K5 Knob Series", red ring; AliExpress, Surenoo, Amazon) and by **Waveshare** as ESP32-S3-Knob-Touch-LCD-1.8 (blue ring). | ESP32-S3R8 + ESP32-U4WDH; 1.8″ ST77916 360×360 QSPI (Guition says 1.85″); CST816S touch; two encoders, one per MCU; PCM5100A line out, through 470 Ω to a 3.5 mm jack (no amp); CH445P I2S switch (V2 only); DRV2605L + LRA; MSM261D4030H1CPM PDM mic; 4-bit SDMMC microSD. A daughter board carries a TP4055 charger, an MX1.25 battery socket, a power switch and a USB-C whose two orientations reach different MCUs. No IMU, no USB hub. **High confidence:** read from the schematics. | Guition `JC3636K518CN_knob_EN.zip` (schematics V1.0/V2.0, bins, demos) from [pan.jczn1688.com](http://pan.jczn1688.com/1/HMI%20display); [Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8); [nkinnan](https://github.com/nkinnan/Waveshare-ESP32-S3-Knob-Touch-LCD-1.8_and_Guition-K5-Knob-Series-JC3636K518) |
| `JC3636W518C` | 太极派, "Taiji Pi": Guition's 1.8″ round board **without a knob** | Guition (AliExpress, Surenoo; about $20) | ESP32-S3R8 only; ST77916 360×360; CST816; PCM5100A on GPIO16/17/18 with XSMT mute on GPIO48; I2S/PDM mic on GPIO42/45/46 (the mic part changed after batch 2528); TF card; Qi wireless charging. **Medium confidence:** gathered from community sources. | [xiaozhi-esp32 `taiji-pi-s3`](https://github.com/78/xiaozhi-esp32/tree/main/main/boards/taiji-pi-s3) (README: "太极派（JC3636W518）"); [ESP32-audioI2S#800](https://github.com/schreibfaul1/ESP32-audioI2S/issues/800); [HA thread](https://community.home-assistant.io/t/guition-1-8-360x360-esp32-s3-jc3636w518-smart-display/750515) |
| `TAIJI VIEWE PI` | 太极优派, an upgraded Taiji Pi | 萨纳兰的黄昏 | **Has a gyroscope/IMU**: G_METER is "only on devices with a gyroscope (太极优派)". IMU part, knob and battery not found *(unverified)*. | [p=119](https://pressf5.run/?p=119), [p=186](https://pressf5.run/?p=186), demo [BV1FjV3zEEHK](https://www.bilibili.com/video/BV1FjV3zEEHK/) |
| `TAIJI KNOB HUB` | 旋钮HUB, a knob combined with a USB hub | 萨纳兰的黄昏 | Has S3 audio: the manual says the alarm MP3 plays on "太极旋钮v2、旋钮hub". It is probably the device later documented as **HALO TOUCH** *(unverified link)*: USB 2.0 hub (USB-C upstream; 2×A and 1×C downstream, the C shared with the flashing port), TF reader, 100 Mbit Ethernet, per-port V/I/W monitor with over-current cut-off, PC/PD power switch, 360×360 screen, knob, mic, and Surface Dial emulation. Hub and monitor chips not found. Guition's Apr 2025 build has `MY_KNOB_HUB` in this slot. | [p=119](https://pressf5.run/?p=119), [p=227](https://pressf5.run/?p=227) |
| `VIEWE HUB` | Probably a hub variant in the 太极优派 line | Unknown | Nothing found *(unverified)* | — |
| `YUYING` | Unknown | Unknown | Nothing found *(unverified)* | — |
| `CarBot` | Probably not a model. It sits among the settings labels (`Local IP`, `CarBot`, `Host IP`), not in the model table. | — | — | string order in the S3 image |

**Source, SDKs and companion apps:**

- **Firmware source:** not published. Guition's zip contains only small LVGL
  demos, Guition's own build of the stock bins (S3 built 10 Apr 2025, with the
  same app), the schematics, an AIDA64 `.rslcd` profile and the TF-card asset
  tree.
- **Firmware and TF-card files** for all three TAIJI models are on the author's
  Baidu Pan (linked from [p=186](https://pressf5.run/?p=186)). Firmware and
  TF-card assets must be version-matched.
- **Companion app:** the author documents a WeChat mini-program
  "萨纳兰的黄昏的控制台" for BLE control and OTA on his other devices. That it
  also drives this firmware's BLE mode is *unverified*.
- **UDP protocol:** no client or documentation found for the discovery/control
  protocol. The web panel is documented only as "open the device IP in a
  browser".

### Waveshare board vs family

- **Same schematic as Guition's:** all five sheets of Waveshare's published
  schematic (`1_LCD&POWER`, `2_ESP32S3-R8`, `3_ESP32-CHIP`, `4_OTHER`, `5_DAC`)
  are byte-identical (same SHA-1) to Guition's JC3636K518 **V2.0** sheets.
  nkinnan reports small PCB layout differences (rotated or missing resistors,
  different packages).
- **Power daughter board:** neither vendor publishes it for V2.0. Guition's
  V1.0 `JC3636K518_Power` sheet shows it: the TP4055 charger, the MX1.25 VBAT
  socket, the power path (a DSK34 diode from VBUS, plus a SI2301 P-FET from
  VBAT), the power switch, and the PJ-342C jack.
- **V1.0 vs V2.0:** V1.0 has no CH445P and no S3 I2S; the PCM5100A is driven
  only by the ESP32 (IO25/26/27), and the ESP32 has its own USB-UART bridge.
  V2.0 adds the CH445P and S3 I2S on GPIO39/40/41. That matches the author's
  "太极旋钮v2 has S3 audio" and is why MUSIC works on this board.
- **Firmware:** Waveshare's S3 V1.2 (built 29 May 2025) and Guition's (10 Apr
  2025) are the same app. Only the model table differs: Guition's has
  `MY_KNOB_HUB` and no `JC3636W518C`. The two ESP32 images are the same IDF 5.4
  project `TAIJI_KNOB_32`, built 14 Apr and 18 Apr 2025.
- **Don't trust Guition's demo `pincfg.h` for this board.** Its audio, mic and
  LCD pins are copied from the Taiji Pi (JC3636W518); only its touch and
  encoder pins match the knob.

### Family features on this board

| Feature | Hardware it needs | On this board? |
|---|---|---|
| `G_METER` | IMU | **No.** There is no IMU on any sheet. G_METER is 太极优派-only. |
| `USB HUB` | USB hub chip, port power monitor | **No.** USB-C goes straight to one MCU, depending on cable orientation. |
| Battery mV / "Low batt." | Battery and ADC | **Yes.** `BATT_ADC` is GPIO1, read through a 10 k/10 k divider on the switched "5V" rail. With USB unplugged that rail is VBAT, through the P-FET. The battery is optional (Waveshare sells versions with and without one). |
| `READER` | The author limits it to devices with a built-in battery | Yes (no special hardware) |
| `SPECTRUM` | PDM mic | Yes (GPIO45/46) |
| Haptics | DRV2605 | Yes (U13, on the touch I2C bus) |
| `MUSIC`, timer alarm sound | S3 I2S to the DAC | Yes (V2.0 boards only). The player only plays 16-bit 44.1/48 kHz. |
| `USB MSC` | S3 native USB, SD | Yes (one cable orientation) |
| `TOMATO`, music-box theme | Knob | Yes |
| `AIDA64`, `WEATHER` | WiFi | Yes |

## Apps (S3)

Selectable on the device, from the web panel (`/docmd?cmd=100+id`), and as the
boot app (`800+id`).

| App | What it does | Content source |
|---|---|---|
| `BLUEAUDIO` | Bluetooth speaker screen: A2DP playback on the ESP32, with title/artist/album and cover art relayed to the S3 | ESP32 over UART |
| `MUSIC` | Local music player: MP3, FLAC, AAC/M4A, with single, list or random repeat | `/music` on SD |
| `MJPEG` (video) | MJPEG video player, single, list or random repeat | `/mjpeg` on SD |
| `MJPEG` (album) | JPEG photo slideshow: manual or automatic, sequential or random | `/pic` on SD |
| `SPECTRUM` | Audio spectrum visualizer from the PDM mic (`task_mic` feeds `task_fft`) | Mic |
| `AIDA64` | PC hardware monitor: CPU and GPU usage, temperature, clock and fan speed, polled from AIDA64's remote-sensor endpoint (`GET /sse`) at a configured host IP, over an MJPEG background | Network, plus `/aida64` on SD for backgrounds |
| `WEATHER` | Weather clock, taking a city code (or city name) and an API key | Network |
| `TOMATO` | Pomodoro/countdown timer with themes ("Default", "Traveler", "Oronys"), an MJPEG background and an `alarm.mp3` alarm | `/clockbg` on SD |
| `READER` | Text-file reader with selectable fonts (Source Sans, Source Sans Bold, Source Han Serif, or custom) | `/txt` on SD, `/fonts` |
| `G_METER` | G-force meter *(family: 太极优派 only; this board has no IMU)*. Has a "Recording device pose" calibration step | IMU |
| `USB HUB` / `USB_HUB` | USB hub status *(family: knob-hub devices; this board has no hub chip)* | |
| `USB MSC` | Reboots into USB mass storage, exposing the SD card to a PC | SD |
| `SETTING` | Settings, listed below | |

## System features (S3)

- **Networking**
  - **WiFi setup:** an AP-mode hotspot ("Connect to 'My-Ap'") with a captive portal (`/configwifi`, `/wifilist`) that scans and lists networks. Credentials are stored in NVS (`WIFIDATA`).
  - **Boot modes:** AP, WiFi, BLE, or flight mode (radios off). Changing mode reboots.
  - **Time sync:** NTP against `ntp.aliyun.com`, `cn.pool.ntp.org`, `pool.ntp.org` and `time.windows.com`, or the time can be pushed from the browser. Timezone offset is configurable.
  - **Signal test:** shows RSSI.
- **Web control panel** (port 80):
  - `/getstatus` returns JSON: `apps`, `DevModel`, `CityCode`, `CityName`, `HostIP`, `vol`, `bri`, `PlayMode` (bit-packed repeat modes), `Language`.
  - `/docmd?cmd=` takes these commands:

    | Command | Action |
    |---|---|
    | 100+id | Switch app |
    | 300 / 301 | Previous / next item |
    | 400–402 | Video repeat: single, list, random |
    | 410–413 | Photo slideshow modes |
    | 420–422 | Music repeat: single, list, random |
    | 800+id | Set boot app |
    | 900 | Reboot |
    | 903 | Mute/unmute |

  - `/save?volume=|brightness=|hostip=|timestamp=` and `/saveweather?citycode=&apikey=`.
  - The panel is in English and Chinese.
- **UDP control protocol** (lwIP `async_udp`), probably for a companion app:
  - discovery broadcast and reply
  - switch mode, previous/next item, favourite slots ("fav mode")
  - brightness, screen on/off, sleep timer
  - status report, time reset/sync, NTP request
- **Settings screen:**
  - language (EN/ZH), local IP, host IP, volume, brightness
  - screen rotation, dark mode, page-flip effect, timer theme, auto-load app
  - reboot, reboot to MSC, factory reset
  - clear WiFi, clear Bluetooth A2DP pairing, clear BLE HID pairing
  - "About" (model, firmware version, free SRAM/PSRAM, chip temperature, battery mV)
  - engineering mode, a screen test that is exited by power-cycling
- **Status screen:**
  - chip temperature (on-chip sensor), free SRAM/PSRAM
  - battery voltage and a "Low batt." warning (this board has an optional battery and reads it on GPIO1; see [Family features on this board](#family-features-on-this-board))
  - SD card size
- **Haptics:** DRV2605 on the touch I2C bus (`task_haptic`, "Could not find DRV2605").
- **Fonts and assets:** UI and reader fonts loaded from SD (`/fonts/UI_FONT_16.bin`, `/fonts/<name>_<size>.bin`); an optional `boot.mjpeg` boot animation; ring and disk masks for round-screen widgets.
- **USB:** TinyUSB CDC console and MSC mode.

## ESP32 (Bluetooth side)

- **A2DP sink:**
  - SBC audio is buffered through a 32 KB ring buffer into a high-priority I2S task (`BtI2STask`, priority 22).
  - Output is I2S Philips stereo, 16-bit, BCLK25/WS27/DOUT26, 6×240 DMA.
- **AVRCP controller:**
  - Play/pause, next and previous.
  - Title/artist/album metadata, with track-change and play-status notifications.
- **AVRCP 1.6 cover art:**
  - Fetches each track's 200×200 linked thumbnail over OBEX, debounced to one request per 3 s.
  - Relays it to the S3 in 1016-byte packets, acknowledged per packet, at 921600 baud.
- **AVRCP target:** absolute volume.
- **HID device modes:** BLE HID (`TAIJI_KNOB_HID`), and Classic BT HID (`BT_HIDD`) is also linked in. The knob acts as a media/volume controller for a PC or phone.
- **Last-device memory:** stores the last peer's address in NVS, presumably for auto-reconnect, and can clear it.
- **Local input:** knob via the `iot_knob` component (timer-polled GPIO, not PCNT).
- **Power:** light sleep with GPIO (knob) wake-up.

### Inter-MCU UART protocol (Waveshare's, not ours)

- 921600 baud, 8N1.
- ESP32 runs TX=IO23, RX=IO18. S3 runs TX=GPIO38, RX=GPIO48.
- ESP32→S3 frames start with `0xBD`, S3→ESP32 frames with `0xA3`.

| Command | Meaning |
|---|---|
| 1 | Cover-art begin (packet count) |
| 2 | Cover-art data (1-based packet id, then up to 1016 bytes) |
| 3 | Cover-art stopped (reason) |
| 5 | Status request; the S3 answers with command 9 |
| 6 | Metadata string (up to 80 characters) |
| 7, 8 | UI events |

The S3 polls the UART every 30 ms and requests each next packet.

## Implementation details worth knowing

- **LCD:**
  - QSPI at 80 MHz, with the same 184-command ST77916 init table as ours.
  - One draw buffer of 360×60 lines (120 in one mode) in internal RAM, with async DMA flush and CPU byte-swap.
  - LVGL loop sleeps `clamp(lv_timer_handler(), 2, 10)` ms.
- **Backlight:** LEDC PWM on GPIO47, 25 kHz, 7-bit, clamped to 5–100%.
- **Touch:** CST816S on I2C SDA11/SCL12 at 400 kHz, RST10, INT9.
- **S3 I2S:** I2S1, 44.1 kHz, Philips, BCLK39/WS40/DOUT41. Note that both chips use **Philips** slots; we use left-justified (MSB).
- **S3 tasks** (stack, priority, core):
  - `lvgl` 6 KB, prio 8, core 0
  - `io` 4 KB, prio 6, core 1
  - `dac` 4 KB, prio 9, core 1
  - `fft` 12 KB, prio 4, core 1
  - `uart1` 4 KB, prio 5, core 1
  - `haptic` 4 KB, prio 2, core 1
  - `udp` prio 7; `ticker` prio 3
- **PSRAM:** four full-frame 360×360 RGB565 buffers for MJPEG, plus an MJPEG "loop cache" sized from whatever PSRAM is left.

## Candidates for this project

Ordered roughly by value against effort, given what we already have:

1. BLE HID knob mode on the ESP32 (media/volume control for a PC).
2. Haptic detents via the DRV2605.
3. Reconnect to the last A2DP device on boot.
4. Spectrum visualizer from the PDM mic.
5. Light sleep on the ESP32 when idle.
6. A web control panel or UDP control (brightness, volume, source).
7. SD-card music playback (an extra audio source on the S3).
