# Development notes

How to build and flash: [Installing the firmware](flashing.md#4-building-it-yourself).

## Project layout

| Path | Contents |
|---|---|
| `src/` | Firmware entry point, web server and API (`src/web/`), LED drivers (`src/led/`) |
| `web/` | Web UI: HTML, CSS, JS and translations (`web/i18n/`), embedded into the firmware at build time |
| `lib/` | Firmware modules; hardware-independent ones are unit-tested on the host |
| `test/` | Unit tests (`pio test -e native`) |
| `partitions/` | Flash partition tables for 4, 8 and 16 MB boards |
| `docs/` | Plan, user documentation, and the hardware guide per board (`docs/hardware/<board>/`) |
| `tools/` | Helper scripts: web UI bundler, release files (`merge_firmware.py`), power budget calculator, `seed_parts.py` to fill a board with test parts |

## Things worth knowing

- **The UI is part of the firmware.** `tools/embed_web.py` gzips `web/` into `src/generated/WebAssets.h` before every build, so a firmware update always brings the matching UI and there is no separate filesystem upload. The same script checks that every translation (`web/i18n/*.json`) has the same keys as `en.json`.
- **Storage.** Everything the board keeps (`config.json`, the inventory database `inventory.db`, the photos) lives on one FAT partition behind ESP-IDF's wear levelling. FAT rewrites only the changed 4 KB sectors when the database updates a record, where LittleFS would copy the rest of the file each time. On first boot the partition is formatted. Wi-Fi credentials, the web password and the work-light state are in NVS, so backups never contain them.
- **Boards.** The firmware is built for one board at a time, picked by `SB_BOARD` in `platformio.ini` (today only `SB_BOARD_ESP32`, the classic ESP32-WROOM-32). Everything that depends on the chip goes through `lib/Board/src/BoardProfile.h`: the default pin map and pin rules (`lib/Board/src/boards/<board>Pins.h`), the LED output driver (`src/led/StripOutput.h`, which checks that a driver has the methods the grid and the work lights call), the number of LED data lines, and the hardware guide (`docs/hardware/<board>/`). A second board, such as an ESP32-S3, adds its own files next to those and its own environments, and shares the rest: the web page reads the board's limits from `/api/pins` (`limits`) instead of keeping its own copy. `config.json` records the board (`"board": "esp32"`); a config or backup from another board keeps every setting except the pin map, which goes back to the new board's defaults.
- **Memory profiles.** Every limit sized for a board without PSRAM is in `lib/Board/src/MemoryProfile.h`; the `-psram` environments set `SB_PSRAM=1` and get the larger values. Library settings that are macros are in the `[mem_small]` and `[mem_psram]` sections of `platformio.ini`.
- **Flash access pauses the LED strips.** On the ESP32 any flash read or write through the file system switches the flash cache off for a moment, which would cut an LED frame in half. Every such access holds a `led::FlashGuard` (`src/led/OutputCommon.h`); new code that touches the FAT partition or NVS while the strips may run must do the same. The database takes it through its read and write hooks; the order is always the database lock first, then the guard.
- **Log level.** `CORE_DEBUG_LEVEL=3` in `platformio.ini` logs warnings, errors, the boot report and the results of user actions; the heap each boot step took is logged at 3 too (from 0.16.0), so a boot log from the field shows where the memory went. 4 adds the rest of the tuning output (search timings, a heap and stack line every 60s, microphone levels).
- **Upgrading from 0.4.x:** version 0.5.0 replaced the LittleFS partition with the FAT one and made the app slots bigger. Flash it once over USB (an over-the-air update can't change the partition table); the board then starts with a default `config.json`.

## HTTP API

[docs/api.md](api.md).
