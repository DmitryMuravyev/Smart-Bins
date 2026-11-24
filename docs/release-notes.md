Smart Bins firmware for ESP32 boards. Pick the files for your board:

| Board | First install over USB (write at 0x0) | Update from the System page |
|---|---|---|
| ESP32-WROOM-32E/UE-N16R2 (16 MB flash, PSRAM) | `…-esp32-16mb-psram-full.bin` | `…-esp32-16mb-psram-update.bin` |
| ESP32-WROOM-32, 16 MB flash | `…-esp32-16mb-full.bin` | `…-esp32-16mb-update.bin` |
| ESP32-WROOM-32, 8 MB flash | `…-esp32-8mb-full.bin` | `…-esp32-8mb-update.bin` |
| ESP32-WROOM-32, 4 MB flash | `…-esp32-4mb-full.bin` | USB only |

How to write them, from the browser or the command line: [Installing the firmware](https://github.com/DmitryMuravyev/Smart-Bins/blob/main/docs/flashing.md). After that: [User guide](https://github.com/DmitryMuravyev/Smart-Bins/blob/main/docs/user-guide.md).

## Changes

First public release. Not every feature has been tested on real hardware yet, so this is marked as a pre-release; please report problems in [Issues](https://github.com/DmitryMuravyev/Smart-Bins/issues).

What it does:
- A bin grid of up to 64 × 64 bins with 1–8 pixels per bin (up to 1,500 pixels), set up from the browser, with test patterns to check the wiring
- Inventory in SQLite on the board: search as you type, filters, sorting, quick quantity buttons, a photo per part, datasheet links
- Backup and restore of everything as one file, CSV export and import
- Up to 5 workspace light channels (addressable, dimmable white or analog RGB), with an optional motion sensor
- Lighting and music effects (music needs a board with PSRAM and a microphone)
- Wi-Fi setup page, `smartbins.local`, firmware updates from the browser, optional password
- English, Spanish and Russian; light and dark theme

The hardware reference now includes the schematic, BOM and EasyEDA Pro project of the 5/12 V board.
