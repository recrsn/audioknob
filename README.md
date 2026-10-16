# AudioKnob

Firmware that turns the [Waveshare ESP32-S3-Knob-Touch-LCD-1.8](https://www.waveshare.com/) into a desk companion: a wireless speaker front-end with a round touch display, a rotary knob, and a handful of small desk apps.

<p align="center">
  <img src="docs/screenshots/home-now-playing.png" width="240" alt="Home screen with now-playing banner">
  <img src="docs/screenshots/launcher.png" width="240" alt="App launcher">
  <img src="docs/screenshots/weather.png" width="240" alt="Weather app">
</p>

## Features

- **AirPlay 2 and AirPlay 1 receiver** (ESP32-S3), with title/artist metadata, artwork and volume/play-pause sent back to the sender.
- **Bluetooth A2DP sink + AVRCP** (ESP32), with the same now-playing view.
- **One source at a time**: a hardware I2S switch routes whichever chip is playing to the PCM5100A DAC; the last source to start wins.
- **Knob and touch UI** built with LVGL: clock home screen, app launcher, timer, pomodoro, stopwatch, weather, lights, settings.
- **Web settings page** at `http://<device-name>.local/` and Wi-Fi setup through a captive portal.
- **OTA updates** for both chips, with rollback.
- **Desktop emulator** of the UI (SDL2), so UI work needs no hardware.

USB-C audio (UAC2) is planned but not implemented.

## Screenshots

Captured from the desktop emulator, which feeds the UI sample data.

| | | |
|:-:|:-:|:-:|
| ![Home](docs/screenshots/home.png)<br>Home | ![Launcher](docs/screenshots/launcher.png)<br>Launcher | ![Bluetooth](docs/screenshots/bluetooth.png)<br>Bluetooth source |
| ![Timer](docs/screenshots/timer.png)<br>Timer | ![Pomodoro](docs/screenshots/pomodoro.png)<br>Pomodoro | ![Stopwatch](docs/screenshots/stopwatch.png)<br>Stopwatch |
| ![Lights](docs/screenshots/lights.png)<br>Lights | ![Weather](docs/screenshots/weather.png)<br>Weather | ![Settings](docs/screenshots/settings.png)<br>Settings |

## Architecture

The board has two MCUs, each with its own firmware and PlatformIO environment:

| Chip | Env | Role |
|------|-----|------|
| ESP32-S3 (8 MB PSRAM) | `s3` | UI, Wi-Fi, AirPlay, SNTP, weather, I2S output, USB |
| ESP32-U4WDH | `esp32` | Classic Bluetooth: A2DP/AVRCP |

They talk over a UART carrying control and metadata only; audio never crosses it. Shared protocol code lives in `firmware/lib/common/`. [docs/spec.md](docs/spec.md) has the hardware facts, pin map and protocol design; [CLAUDE.md](CLAUDE.md) has per-module notes.

## Building

Requires [PlatformIO](https://platformio.org/).

```bash
pio run -d firmware/s3 -e s3 -t upload       # flash the ESP32-S3
pio run -d firmware/esp32 -e esp32 -t upload # flash the ESP32-U4WDH
pio run -d firmware/s3 -e native -t exec     # run the UI on the desktop
```

The native target needs SDL2 (`brew install sdl2` on macOS) and expects Homebrew under `/opt/homebrew`; adjust `build_flags` in `firmware/s3/platformio.ini` otherwise.

Emulator keys: Up/Down turn the knob, Enter presses it, Esc long-presses it (opens the launcher), `P` cycles sample playback states.

### Configuration

`sdkconfig.defaults.*` in each firmware project hold the defaults. Wi-Fi credentials are entered on the device (captive portal or web page); the `AK_WIFI_SSID` / `AK_WIFI_PASSWORD` Kconfig options are optional development fallbacks and ship empty. Don't commit real values.

## Repository layout

```
firmware/s3/        ESP32-S3 project (PlatformIO + ESP-IDF, plus the native UI emulator)
  src/ui/             LVGL screens, apps, generated fonts/backgrounds (builds natively too)
  src/device/         hardware-only code: airplay/, drivers/, net/, core/
  lib/pair_ap/        vendored AirPlay 2 pairing library
firmware/esp32/     ESP32-U4WDH project (Bluetooth)
firmware/lib/common/  UART protocol shared by both
docs/               spec, stock firmware notes, screenshots
hardware/           OpenSCAD desk stand
tools/              generators for the files in src/ui/assets
```

## License

[MIT](LICENSE). Bundled third-party code and its licenses are listed in [THIRD_PARTY.md](THIRD_PARTY.md).
