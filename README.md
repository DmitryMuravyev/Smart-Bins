# Smart Bins

An addressable parts library for the home lab, built on the ESP32.

Smart Bins is a grid of plastic storage bins, each one lit by 1–3 addressable LEDs. A web interface keeps an inventory of your components in a SQLite database on the ESP32's flash. Search for a part (for example `STM32`), and the bin where it's stored lights up.

> **Status:** early development. See [docs/PLAN.md](docs/PLAN.md) for the implementation plan and progress.

## Features (planned)

- Configurable bin grid of any size (X × Y), 1–3 LEDs per bin, flexible strip wiring
- Inventory in SQLite on LittleFS, with instant global search and sorting by manufacturer, category and part number
- Component photos (JPG)
- Database backup (download) and restore from the browser
- 2–3 independently controlled addressable strips for workspace lighting
- Lighting effects: fire, sparks, relax, music-reactive (I2S microphone), demo
- Wi-Fi setup portal, `smartbins.local` via mDNS, over-the-air firmware updates

## Hardware

- ESP32-DevKitC (ESP32-WROOM-32), 4/8/16 MB flash
- WS2812 / WS2812B / SK6812 LED strips
- 74HCT245 level shifter, P-channel MOSFET power switches, 5 V power supply
- Optional: INMP441 I2S microphone, microSD card

Wiring and schematics will be in `docs/` (Task 2 of the plan).

## Development

Built with [PlatformIO](https://platformio.org/) in VS Code, using the Arduino framework for ESP32 (Arduino-ESP32 core 3.x via the [pioarduino](https://github.com/pioarduino/platform-espressif32) platform).

1. Install VS Code and the **PlatformIO IDE** extension (VS Code suggests it when you open this folder).
2. Open the repository folder in VS Code. PlatformIO downloads the toolchain on first build.
3. Choose the environment that matches your board's flash chip in the PlatformIO status bar: `esp32-4mb`, `esp32-8mb` (default) or `esp32-16mb`.
4. Connect the board over USB and run **Upload and Monitor**.

The boot log shows chip, flash, memory and partition information. If the detected flash size differs from the build environment, it prints a warning naming the right one.

From the command line:

```sh
pio run -e esp32-8mb -t upload -t monitor   # build, flash and open the serial monitor
pio test -e native                          # host-side unit tests, no hardware needed
```

### Project layout

| Path | Contents |
|---|---|
| `src/` | Firmware entry point |
| `lib/` | Firmware modules; hardware-independent ones are unit-tested on the host |
| `test/` | Unit tests (`pio test -e native`) |
| `partitions/` | Flash partition tables for 4, 8 and 16 MB boards |
| `docs/` | Plan, hardware and user documentation |

## License

[MIT](LICENSE) © 2026 Dmitry Muravyev
