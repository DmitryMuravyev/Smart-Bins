# Smart Bins: Implementation Plan (draft v0.4)

An ESP32-based addressable parts library for a home lab: a grid of bins, each with 1–8 addressable pixels, a SQLite inventory stored on flash, a web UI for search/add/edit with photos, backup/restore, workspace lighting and LED effects. Open source under MIT.

Status: **proposal**. Every stack choice below is a default that can be changed before the task that depends on it starts. Each task ends with a demo you can verify on hardware, and nothing moves to the next task without your confirmation.

---

## 0. Decisions so far (v0.4, updated 2026-10-29)

| # | Topic | Decision |
|---|---|---|
| 1 | Repository | Private GitHub repo `smart-bins` for now (MIT), made public later. Dmitry creates it empty; Claude fills it. |
| 2 | Board | **Updated 2026-11-01:** ESP32-WROOM-32E/UE-N16R2 (16 MB flash, 2 MB PSRAM), firmware env `esp32-16mb-psram`; grid data moved to GPIO14/15 and PS-ON to GPIO2 to free GPIO16/17 for the PSRAM. Originally: ESP32-DevKitC replica (ESP32-WROOM-32, no PSRAM), still supported by the other envs. Flash size to be confirmed in Task 1 (`esptool.py flash_id`): many DevKitC clones ship with 4 MB, so a 4 MB partition table is added as a fallback (photos would then need the optional microSD). |
| 3 | LED architecture | See "LED and power architecture" below. |
| 4 | Work lights | Two options (updated 2026-10-29): **1.** an analog RGB strip on W1–W3 plus white strips on W4–W5; **2.** W1–W3 each set to addressable data or white PWM, plus white strips on W4–W5. See "Work-light channels" below. |
| 5 | Pin priority | Main addressable grid → workspace illumination → auxiliary lights and/or microphone. |
| 6 | LED types | Widest possible range: 5 V WS2812/WS2812B/SK6812 and 12/24 V WS2811/WS2815, chosen in the config. Dmitry's build: 12 V strips (WS2811 or WS2815). The grid supply voltage is a design parameter (5, 12 or 24 V). |
| 7 | Grid | Dmitry's build: 10 × 6 bins, 2 pixels per bin (configurable 1–8). All bins identical. Optional per-row/per-column overrides if they stay simple (see Task 4); multi-cell bins dropped. |
| 8 | Smooth transitions | Every on/off and brightness change of the grid and the work lights fades smoothly (default ~400 ms). Exceptions: lighting effects, and hard power cuts (fault, overcurrent, power loss). |
| 9 | Locate highlight | Configured in the UI: duration (e.g. 10 s to "until dismissed"), color, and mode: **steady** (fade in/out), **slow pulse**, **fast pulse** (smooth). |
| 10 | Part data | One bin per part, many parts per bin; default field list (section 3). Quick +/− quantity buttons on every part card. |
| 11 | Photos | Resized in the browser to **640 px** (long side) when stored on flash, **800 px** when a microSD card is present. Firmware constants, not exposed in the UI. |
| 12 | Backup | Both options: database only, or database + photos + config in one file. |
| 13 | Languages | UI in English (mandatory), Spanish and Russian. README in all three languages. |
| 14 | Security | Optional web UI password (off by default). |
| 15 | Prototype | Breadboard prototype with discrete transistor level shifters; the final design keeps the 74HCT245. Parts list still open. |

### LED and power architecture (chosen)

- **Separate data outputs, one shared driver.** Bin grid on 1–2 data pins (the grid can be split into two segments, e.g. top half / bottom half, so a long run keeps a high frame rate and one damaged strip doesn't kill the whole grid). Work lights use the universal channels described below. The firmware's own RMT driver runs each addressable output on its own RMT channel, in parallel.
- **Within the grid, rows are chained serpentine** (row 1 left→right, row 2 right→left...) so each segment needs just one data wire. The mapper supports progressive wiring and any start corner too, so physical layout stays flexible.
- **Level shifting:** one 74HCT245 (8 channels) converts the 2 grid lines and the 5 work-light lines (W1–W5) from 3.3 V to 5 V, with a 330 Ω series resistor on each output.
- **Power switching with MOSFETs:** each addressable power domain (bin grid, addressable work strips) gets a high-side P-channel MOSFET (AO4407A, driven by a small NPN, with a gate divider chosen for 5, 12 or 24 V) controlled by a GPIO. This cuts the idle current of every LED (~1 mA each when "off"), lets firmware fully de-power strips on fault/overcurrent, and keeps strips off during ESP32 boot so they never flash random colors. High-side rather than low-side switching, so data lines never back-power the LEDs through a floating ground.
- **Supply control (optional):** with a supply that has a 5 V standby output and remote on/off (Dmitry's build: Mean Well EPP-300-12), the ESP32 runs from standby 5 V, turns the main output on through PS-ON before any light needs it and off after the last fade-out, and watches PG to hold LED data until the output is up and to detect power failure. The P-MOSFET switches then become optional.
- **Protection:** LED supply (5, 12 or 24 V) sized from the power budget, 5 V for the ESP32 from the supply's standby output or a buck converter when the strips run at 12/24 V, a fuse per domain, 1000 µF bulk capacitor at each strip input, power injection every ~1 m or per row, common ground between ESP32 and LED supply, firmware current limit (estimated from the pixel data, per grid and per addressable work strip).
- **Smooth transitions:** the LED engine fades every on/off and brightness change (decision 8). Power switches turn on before a fade-in starts and off only after a fade-out ends.

### Work-light channels

All five work channels are **universal**: each GPIO is wired to both a low-side N-MOSFET (white or analog RGB strips) and a 74HCT245 input (addressable strips). An analog RGB strip uses W1–W3 together. Each channel's mode is a setting in `config.json` (editable from the web UI in Task 14, no reflashing).

| Option | W1 | W2 | W3 | W4, W5 |
|---|---|---|---|---|
| 1. Analog RGB strip + other strips | R | G | B | addressable **or** white PWM |
| 2. Universal channels | addressable **or** white PWM | addressable **or** white PWM | addressable **or** white PWM | addressable **or** white PWM |

- **White / analog RGB:** LEDC hardware PWM, 12-bit at ~19.5 kHz (no visible flicker on cameras, no audible whine), gamma-corrected fades. Logic-level N-MOSFET per channel (AO3400A up to ~3 A, IRLB8721 for more), 100 Ω gate resistor and 10 kΩ pull-down so strips stay dark while the ESP32 boots. Works with 5, 12 or 24 V strips.
- **Addressable:** the same GPIO through the 74HCT245, powered via the work-light P-MOSFET switch. Any mix is allowed in option 2, e.g. W1 addressable + W2, W3 white.
- The microphone has its own pins, so it never competes with the work lights.

### Pin map (ESP32-DevKitC, WROOM-32E/UE-N16R2 or WROOM-32)

| Priority | Function | GPIO | Notes |
|---|---|---|---|
| 1 | Grid data A / B | 14, 15 | via 74HCT245; GPIO16/17 are left to the PSRAM |
| 1 | Grid power enable | 32 | P-MOSFET high-side switch |
| 2 | Work-light channels W1, W2, W3 | 27, 13, 4 | universal (PWM or addressable) |
| 2 | Work-light addressable power enable | 33 | P-MOSFET, only needed for addressable work strips |
| 2 | Supply on/off (PS-ON), power good (PG) | 2, 39 | optional, for supplies with remote control such as the EPP-300-12 |
| 3 | White channels W4, W5 | 21, 22 | PWM only |
| 3 | I2S microphone (INMP441) SCK, WS, SD | 26, 25, 34 | GPIO34 is input-only, fine for data in |
| 3 | Analog microphone (MAX9814) alternative | 35 | ADC1, replaces I2S mic |
| – | Optional microSD (photos on 4 MB boards) | 18, 19, 23, 5 | VSPI |
| – | Button (clear highlight / factory reset) | 0 | on-board BOOT button |
| – | Status LED | 2 | on-board LED on most DevKitC boards; off when PS-ON uses GPIO2 |
| – | Spare | 15, 36 (input-only) | 12 is a strapping pin, never used |

Final map is confirmed with the schematic in Task 2.

---

## 1. Proposed stack (defaults)

| Area | Default | Why / alternatives |
|---|---|---|
| Build | **PlatformIO in VS Code**, Arduino framework on ESP32 | Best VS Code workflow. Uses the `pioarduino` platform to get Arduino-ESP32 core 3.x (the official PlatformIO platform is stuck on core 2.x). Alternative: ESP-IDF + Arduino as component. |
| MCU | ESP32-DevKitC with WROOM-32E/UE-N16R2 (**2 MB PSRAM**, 16 MB flash); plain WROOM-32 (no PSRAM, 4/8/16 MB) still supported | One `SB_PSRAM` switch (`lib/Board/src/MemoryProfile.h`, set by the `-psram` envs) picks the memory limits: without PSRAM they assume ~300 KB usable heap; with it, caches, buffers and parallel requests are larger. |
| Flash layout | Custom partition tables. 8 MB and 16 MB: 2 × 2.25 MB OTA app slots + rest FAT (≈3.4 MB / ≈11.4 MB). 4 MB fallback: one 2.25 MB app slot + ≈1.6 MB FAT (config and database only, photos on microSD); updated over USB. | OTA updates over Wi-Fi from day one. The firmware is ~1.7 MB with SQLite (v0.5.0), so two copies plus storage no longer fit in 4 MB; the slots were sized with room for the remaining tasks. |
| Filesystem | **FAT on ESP-IDF wear levelling** for everything (changed in Task 7, was: LittleFS) | LittleFS copies the rest of a file whenever its middle changes, which every SQLite commit does: seconds per edit and extra wear on a large database. FAT rewrites only the changed sectors. One partition for config, database and photos means no fixed split, and the same code serves the optional microSD (also FAT). Power-loss safety: SQLite's journal for the database, write-then-rename with recovery for `config.json`. |
| Database | **SQLite 3** via `siara-cc/esp32_arduino_sqlite3_lib`, DB file on the FAT partition | Without PSRAM: 2 KB pages, 8 KB page cache, 4 KB journal write buffer (4 KB pages made write peaks too large). With PSRAM: 4 KB pages, 512 KB cache, 8 KB write buffer. Schema version in `PRAGMA user_version`. Search with `LIKE` across all text fields; FTS5 only if needed. |
| LEDs | **Own RMT output driver** (ESP-IDF `rmt_tx`), WS2812/WS2812B/SK6812/SK6812 RGBW (5 V), WS2811/WS2815 (12/24 V) | Pin, chipset timing, color order and inverted output are chosen at runtime from `config.json`, which FastLED's compile-time controllers can't do. FastLED's color math may still be used for effects (Task 12). |
| Web server | **ESPAsyncWebServer** (maintained `ESP32Async` fork) + WebSocket for live updates | Streams files without buffering in RAM. |
| Web UI | Vanilla JS + small CSS, no framework; strings in per-language JSON files (en, es, ru). Gzipped at build time by `tools/embed_web.py` and **embedded in the firmware** (changed in Task 6, was: files on LittleFS) | Small and fast on ESP32. One firmware image (also over the air) always carries the matching UI, no separate filesystem upload, and the data partition stays free for the inventory and photos. |
| Backup | **Plain tar** archive (manifest, config.json, inventory.db, photos), streamed both ways a 512-byte block at a time; the database file is copied while writes are held (rollback journal: the file is consistent between transactions); restore swaps the file in and keeps the old one | No RAM for compression or a copy; every OS opens tar. |
| Photos | JPG, **resized in the browser** to 640 px (flash) or 800 px (microSD) before upload, stored as `/photos/<part_id>-<6 hex>.jpg` plus a 128 px `-t.jpg` thumbnail (a new name per photo, so URLs can be cached forever) | Keeps ESP32 RAM and flash use predictable. |
| Wi-Fi | Station mode + fallback access point with captive setup page, mDNS `http://smartbins.local` | No hard-coded credentials in source. |
| Audio | **INMP441 I2S digital microphone** + `arduinoFFT` | Cleaner than analog mics on the noisy ESP32 ADC. Analog MAX9814 as fallback option. |
| Tests / CI | PlatformIO `native` env for unit tests of pure logic (grid mapping, effects, query building); GitHub Actions builds firmware on every push | |
| License | MIT. Dependencies checked for compatibility (FastLED MIT, SQLite public domain, ESPAsyncWebServer LGPL-3.0: fine for an open-source project, noted in README) | |

## 2. High-level architecture

```
 Browser (search / add / edit / photos / settings / effects)
        │ HTTP REST + WebSocket
 ┌──────▼─────────────────────────────────────────────────┐
 │ ESP32                                                  │
 │  WebServer ── REST API ── InventoryDb ─── SQLite (FAT)
 │      │                         │                       │
 │      │                    PhotoStore (FAT / SD)        │
 │      │                                                 │
 │  ConfigService (config.json) ── GridMapper (bin x,y → pixel idx)
 │                                    │                   │
 │  LedEngine (RMT, render task)     ◄┘                   │
 │    ├─ Locate/highlight layer (top priority, fades)     │
 │    ├─ Effects: fire, sparks, relax, music, demo        │
 │    └─ Work lights W1..W5 (PWM or addressable)          │
 │  AudioService (I2S mic → FFT → bands)                  │
 │  WiFi / mDNS / OTA / BackupService                     │
 └────────────────────────────────────────────────────────┘
```

LED rendering runs in its own FreeRTOS task on core 1; Wi-Fi and the web server stay on core 0 so the LEDs don't stutter during uploads.

## 3. Draft data model

- `parts`: id, part_number, manufacturer, category, description, package, value, unit, datasheet_url, notes, quantity, bin_id, photo (filename), created_at, updated_at
- `bins`: id, x, y, label, in_grid (0 for a bin left outside after the grid shrank, kept while parts are in it)
- Categories and manufacturers are free text, offered for autocomplete from the values in use
- Schema version in `PRAGMA user_version` (for migrations)

One part lives in one bin; one bin can hold many parts.

---

## 4. Sub-tasks

Each task lists its scope and how you'll verify it. Tasks are ordered so something works on real hardware early.

### Task 0: Decisions and repository
- Agree on answers to the questions in section 5.
- Create the GitHub repository: README, MIT LICENSE, `.gitignore`, `docs/`, this plan as `docs/PLAN.md`.
- **Done when:** repo exists and the plan is committed.

### Task 1: Project skeleton and build
- PlatformIO project (`pioarduino` platform), environments `esp32-4mb`, `esp32-8mb`, `esp32-16mb`, `native` (tests).
- Custom partition CSVs for 4 MB, 8 MB and 16 MB, LittleFS mounted at boot, serial logging with levels.
- Source layout (`src/`, `lib/`, `web/` for web files, `test/`), `.clang-format`, GitHub Actions build.
- **Verify:** firmware builds in CI; on your board it boots, prints flash size, chip info and free LittleFS space.

### Task 2: Hardware reference design
- Wiring doc and schematic for the architecture in section 0: LED data pins, 74HCT245 level shifter, P-MOSFET power switches per domain (5/12/24 V), supply sizing, breadboard level-shifter notes, injection points, fuse, capacitor and series resistor, mic wiring, optional SD card and button.
- Default pin map that avoids strapping and flash pins.
- Power budget calculator (LEDs × mA) and default brightness/current limit.
- **Verify:** you review the doc against your parts.

### Task 3: Configuration and Wi-Fi
- `config.json` on LittleFS: grid width/height, pixels per bin (1–8), strip wiring order (row-by-row or serpentine, start corner), LED pin(s), chipset (WS2812/WS2812B/SK6812/WS2811/WS2815), color order, strip voltage, work-light outputs, brightness and current limit, device name.
- Wi-Fi station + fallback AP with a minimal setup page, mDNS, OTA update. Wi-Fi credentials in NVS, not in `config.json`, so backups never contain them.
- BOOT button: hold 5 s to forget Wi-Fi, 10 s for a factory reset. Status LED shows portal / connecting / connected.
- **Verify:** fresh board opens an AP, you enter Wi-Fi credentials, it joins your network and answers at `smartbins.local`; firmware updates over the air.

### Task 4: LED driver and grid mapping
- `GridMapper`: bin (x, y) → list of pixel indices for any grid size, pixels per bin, serpentine or progressive layout, start corner. Optional per-row / per-column overrides (bins in a row, pixels per bin, skipped pixels between bins) if they stay simple; otherwise deferred. Unit-tested in `native`.
- `LedEngine` render task with layers (locate highlight above effects), global brightness and power limit, and smooth fades for every on/off and brightness change (decision 8).
- Locate highlight modes: steady (fade in/out), slow pulse, fast pulse; duration and color as settings.
- Test patterns: walk every bin, chain (one pixel along the wiring), row/column sweep, color order check; locate any bin by x/y. Temporary REST endpoints until the UI (Task 6).
- Optional inverted data output for single-transistor level shifters (`grid.invertData`).
- **Verify:** on your strip, the test walk lights bins in the correct physical order.

### Task 5: Workspace lights
- Channels per section 0: W1–W5 universal (off / white PWM / addressable), W1–W3 also the RGB group. LEDC PWM (12-bit, 19.5 kHz) with gamma correction, the same RMT driver as the grid for addressable work strips, smooth fades on every change.
- On/off, brightness, color and white tone, simple per-strip effects; state persisted across reboots.
- **Verify:** each light controllable independently via a temporary REST call.

### Task 6: Web server and REST API foundation
- ESPAsyncWebServer, gzipped UI files embedded in the firmware, JSON REST conventions, error format, WebSocket channel for state pushes.
- Endpoints so far: system info, restart, config get/set, LED test, locate bin and locate defaults (mode, duration, color), work lights.
- Basic UI shell (navigation, status bar) with no inventory yet, and the translation mechanism: English, Spanish and Russian string files, language picker (per browser).
- Pages so far: work lights (on/off, level, color wheel and white tone sliders that fit a phone in portrait, effects), grid (tap a bin to locate it, locate mode/duration/color, test patterns), settings (language, password, raw config editor), system info.
- Optional password (off by default) protecting the UI, the API and over-the-air updates (HTTP digest auth; the Wi-Fi setup page stays open on the setup access point; a 10 s BOOT factory reset removes it).
- **Verify:** UI loads on phone and desktop; "locate bin (x,y)" lights the right bin.

### Task 7: SQLite database layer
- Integrate SQLite, tuned for low RAM (page cache, temp store in memory limited).
- Schema + migrations by `schema_version`; `PartsService` CRUD; `bins` table synced to grid config.
- **Verify:** API calls create/read/update/delete parts; data survives reboot; a test inserts e.g. 2,000 parts and measures query time and free heap.

### Task 8: Search and sorting
- Global search: one query string matched (case-insensitive, substring) across all text fields, e.g. `STM32` finds part numbers, descriptions and notes.
- Sort by manufacturer, category, part number (asc/desc), filters by category/manufacturer, pagination.
- Results streamed as JSON to keep RAM flat.
- **Verify:** "search as you type" responds fast on the real board with your test data set.

### Task 9: Inventory web UI
- Search page with instant results, sortable table/card view, filters.
- Quick **+ / −** quantity buttons on every part card (no need to open the edit form).
- Add/edit/delete part form with autocomplete for manufacturer and category.
- "Locate" button lights the bin (optionally auto-on when search narrows to one bin); visual grid view showing bin contents and empty bins; click a bin to see its parts.
- **Verify:** end-to-end use from a phone in the lab.

### Task 10: Photos
- Upload JPG (camera or file) → browser resizes to 640 px (800 px with microSD) and compresses → streamed to storage.
- Thumbnails in results, full view on detail page, delete/replace; storage usage shown.
- Optional microSD as photo store.
- **Verify:** add photos from your phone; flash usage stays within budget.

### Task 11: Backup and restore
- Download the database file safely (DB closed or checkpointed during copy).
- Full backup option: database + photos + config as one archive file.
- Restore with validation (format, schema version) and automatic pre-restore backup; migrations applied on older backups.
- Optional CSV export/import for editing in a spreadsheet.
- "Move photos to card" on the System page, shown when a microSD card is present and photos are still in flash: copy each, verify the copy, then delete it from flash. Manual, not at boot, so a failing card can't cost the flash copies.
- **Verify:** backup, wipe, restore round-trip gives identical data.

### Task 12: Lighting effects
- Effect framework (parameters: speed, intensity, palette, brightness), mapped onto the 2D bin grid.
- Effects: **fire**, **sparks**, **relax** (slow breathing color fields), **demo** (cycles all effects).
- Locate highlight always overrides effects; effects can be scheduled to stop after N minutes.
- **Verify:** each effect runs smoothly at the target frame rate on your grid.

### Task 13: Music-reactive lighting
- I2S microphone input, FFT into frequency bands, beat detection, auto-gain.
- Music effects: spectrum bars across columns, beat flashes, volume pulse.
- Works without a microphone fitted (effect hidden/disabled).
- **Verify:** effects follow music in the room.

### Task 14: Settings and effects UI
- Hardware page: per-channel mode (off / white PWM / RGB group / addressable) and GPIO assignment for every function, validated against the board (flash, input-only and strapping pins rejected, no double use), applied after a reboot. Holding the BOOT button for 10 s (factory reset) restores the default pin map in case a bad setting leaves the device unreachable. (Holding BOOT *during* power-up isn't possible: it puts the ESP32 into its flashing mode.)
- Settings page: grid and LED config editor with live preview, Wi-Fi, device name, brightness/current limit, fade time, locate highlight (duration, color, steady / slow pulse / fast pulse), language, password, OTA upload.
- Effects page: pick effect, parameters, timer; work-light controls.
- **Verify:** reconfigure the grid size from the browser and the mapping updates without reflashing.

### Task 15: Hardening and release 1.0
- Watchdog, heap monitoring, error pages, input validation.
- Documentation: build/flash guide, wiring, user guide, screenshots; README in English, Spanish and Russian; GitHub release with prebuilt binaries for 4, 8 and 16 MB and the PSRAM module.
- **Verify:** fresh-flash test from the release binaries following only the docs.
- **Status (2026-11-01):** done except the final check. Restart reason on the System page and in the log, a main-loop watchdog (60s), a 404 page, quieter log (tuning output at debug level), singular forms in the UI. Docs: `docs/flashing.md`, `docs/user-guide.md`, `docs/api.md`, `docs/development.md`, README in English, Spanish and Russian, screenshots. CI builds a full image (write at 0x0) and an update file per board; a `v*` tag drafts the GitHub release. Left for the ESP32-WROOM-32E/UE-N16R2 modules: test the PSRAM build on them, then the fresh-flash test from a release, then tag v1.0.0.

### Possible later extras (not planned unless you want them)
Barcode/QR scanning in the UI to find parts, low-stock alerts, MQTT / Home Assistant integration, physical "find" button or rotary encoder, multiple grids/cabinets, printable bin labels.

---

## 5. Clarifying questions

All answered (see section 0). Still open:
- **Parts list:** Dmitry is assembling the shopping list; the BOM in `docs/hardware/README.md` can change with it.
- **Microphone:** no module yet (default INMP441 I2S).
