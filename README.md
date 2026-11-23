# Smart Bins

[![Español](https://img.shields.io/badge/Espa%C3%B1ol-8A2BE2)](README.es.md)
[![Русский](https://img.shields.io/badge/%D0%A0%D1%83%D1%81%D1%81%D0%BA%D0%B8%D0%B9-8A2BE2)](README.ru.md)
[![Build](https://github.com/DmitryMuravyev/Smart-Bins/actions/workflows/build.yml/badge.svg)](https://github.com/DmitryMuravyev/Smart-Bins/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/DmitryMuravyev/Smart-Bins)](https://github.com/DmitryMuravyev/Smart-Bins/releases/latest)

An addressable parts library for the home lab, built on the ESP32.

Smart Bins is a grid of plastic storage bins, each one lit by 1–8 addressable LEDs. A web page on your phone keeps the inventory of your components in a SQLite database on the ESP32's flash. Search for a part (for example `STM32`), and the bin where it is stored lights up.

<p>
<img src="docs/images/search.png" width="200" alt="Searching the inventory">
<img src="docs/images/inventory-dark.png" width="200" alt="Part cards, dark theme">
<img src="docs/images/grid.png" width="200" alt="Lighting bins on the grid">
<img src="docs/images/lights.png" width="200" alt="Work lights">
</p>

## Features

- A bin grid of up to 64 × 64 bins with 1–8 pixels per bin and up to 1,500 pixels in all (for example 64 × 23 bins with 1 pixel each, or 187 bins with 8), wired in a zigzag or row by row, on one or two data lines; set up from the browser, with test patterns to check the wiring
- Inventory in SQLite on the board: search as you type across every field, filters, sorting, **+ / −** quantity buttons, a bin view of the search results
- A photo per part, taken with the phone's camera, stored in flash or on a microSD card
- Backup and restore of everything (parts, photos, settings) as one file, CSV export and import
- Workspace lights: up to 5 strips, each addressable (up to 1,000 pixels) or dimmable white, or an analog RGB strip on three of the channels; every change fades; an optional motion sensor switches them
- Lighting effects on the grid (fire, sparks, relax, demo) and music effects with an I2S or analog microphone (boards with PSRAM)
- Wi-Fi setup page, `smartbins.local` address, firmware updates from the browser, optional password
- English, Spanish and Russian; light and dark theme

## Hardware

- An ESP32-DevKitC with an **ESP32-WROOM-32E/UE-N16R2** (16 MB flash, 2 MB PSRAM), or a plain ESP32-WROOM-32 with 4, 8 or 16 MB flash. The firmware reads the flash at 80 MHz in QIO mode, which every Espressif WROOM-32 / WROOM-32E module supports; on a board with a bare ESP32 chip and other flash, or one that uses GPIO9/10 for something else, set `board_build.flash_mode = dio` (and if needed `board_build.f_flash = 40000000L`) in `platformio.ini` ([hardware guide](docs/hardware/esp32-wroom/README.md#flash-mode))
- Addressable LED strips: WS2812 / WS2812B / SK6812 (5 V) or WS2811 / WS2815 (12/24 V)
- A 74HCT245 level shifter, MOSFET power switches, an LED power supply (5, 12 or 24 V)
- Optional: INMP441 (I2S) or MAX9814 (analog) microphone, microSD card, LD2410C or PIR motion sensor
- Room for parts: the database takes about 200 bytes per part (300 real parts: 56 KB), and it may fill about half of the flash storage, so that a restore has room for the new copy. That is roughly **3,000 parts with 4 MB flash, 8,000 with 8 MB and 25,000 with 16 MB**. Photos go on the microSD card; without one they share the same space, at about 100 KB each.

Schematics, wiring, parts list and power budget: [docs/hardware/esp32-wroom/README.md](docs/hardware/esp32-wroom/README.md).

## Getting started

1. **Build the circuit:** [Hardware reference](docs/hardware/esp32-wroom/README.md).
2. **Install the firmware:** download the file for your board from the [releases](https://github.com/DmitryMuravyev/Smart-Bins/releases) and write it from the browser, no tools needed: [Installing the firmware](docs/flashing.md).
3. **Connect it to Wi-Fi** and open **http://smartbins.local/**: [User guide](docs/user-guide.md).

## Documentation

| Document | What's in it |
|---|---|
| [User guide](docs/user-guide.md) | First start, the pages, setting up the grid and lights, backups, troubleshooting |
| [Installing the firmware](docs/flashing.md) | Which file for which board, flashing from the browser or the command line, updates, building from source |
| [Hardware reference](docs/hardware/esp32-wroom/README.md) | Circuit, pin map, power, parts list |
| [HTTP API](docs/api.md) | The JSON API the page uses, for scripts and home automation |
| [Development notes](docs/development.md) | Project layout and how the pieces fit |
| [Plan](docs/PLAN.md) | Decisions and the task list |

## License

[MIT](LICENSE) © 2026 Dmitry Muravyev
