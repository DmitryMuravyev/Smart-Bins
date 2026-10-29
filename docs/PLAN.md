# Smart Bins: Implementation Plan (draft v0.3)

An ESP32-based addressable parts library for a home lab: a grid of bins, each with 1–3 addressable LEDs, a SQLite inventory stored on flash, a web UI for search/add/edit with photos, backup/restore, workspace lighting and LED effects. Open source under MIT.

Status: **proposal**. Every stack choice below is a default that can be changed before the task that depends on it starts. Each task ends with a demo you can verify on hardware, and nothing moves to the next task without your confirmation.

---

## 0. Decisions so far (v0.3, updated 2026-10-29)

| # | Topic | Decision |
|---|---|---|
| 1 | Repository | Private GitHub repo `smart-bins` for now (MIT), made public later. Dmitry creates it empty; Claude fills it. |
| 2 | Board | ESP32-DevKitC replica (ESP32-WROOM-32, no PSRAM). Flash size to be confirmed in Task 1 (`esptool.py flash_id`): many DevKitC clones ship with 4 MB, so a 4 MB partition table is added as a fallback (photos would then need the optional microSD). |
| 3 | LED architecture | See "LED and power architecture" below. |
| 4 | Work lights | Configurable per installation (updated 2026-10-29): 3 dimmable white strips, **or** 1 analog RGB strip, **or** 1 addressable RGB strip. Leftover pins drive extra strips. See "Work-light channels" below. |
| 5 | Pin priority | Main addressable grid → workspace illumination → auxiliary lights and/or microphone. |

### LED and power architecture (chosen)

- **Separate data outputs, one shared driver.** Bin grid on 1–2 data pins (the grid can be split into two segments, e.g. top half / bottom half, so a long run keeps a high frame rate and one damaged strip doesn't kill the whole grid). Work lights use the universal channels described below. FastLED drives the addressable outputs in parallel via the ESP32 RMT peripheral.
- **Within the grid, rows are chained serpentine** (row 1 left→right, row 2 right→left...) so each segment needs just one data wire. The mapper supports progressive wiring and any start corner too, so physical layout stays flexible.
- **Level shifting:** one 74HCT245 (8 channels) converts the 2 grid lines and the 5 work-light channel lines from 3.3 V to 5 V, with a 330 Ω series resistor on each output.
- **Power switching with MOSFETs:** each power domain (bin grid, work lights) gets a high-side P-channel MOSFET (e.g. AO3401/IRF9540-class, driven by a small NPN or logic-level N-MOSFET) controlled by a GPIO. This cuts the idle current of every LED (~1 mA each when "off"), lets firmware fully de-power strips on fault/overcurrent, and keeps strips off during ESP32 boot so they never flash random colors. High-side rather than low-side switching, so data lines never back-power the LEDs through a floating ground.
- **Protection:** 5 V supply sized from the power budget, a fuse per domain, 1000 µF bulk capacitor at each strip input, power injection every ~1 m or per row, common ground between ESP32 and LED supply, firmware current limit (FastLED `setMaxPowerInVoltsAndMilliamps`).
### Work-light channels (universal outputs)

Each work-light channel is one GPIO wired to **both** a low-side N-MOSFET driver (for non-addressable strips) and a 74HCT245 input (for addressable strips). The connector you use decides the strip type, and the channel's mode is a setting in `config.json` (editable from the web UI in Task 14, no reflashing), so the same board supports every option without rewiring:

| Option | W1 | W2 | W3 | W4, W5 (auxiliary) |
|---|---|---|---|---|
| 1. Three dimmable white strips | white PWM | white PWM | white PWM | extra white strips |
| 2. One analog RGB strip | R | G | B | extra white strips |
| 3. One addressable RGB strip | addressable data | spare: white PWM | spare: white PWM | extra white strips |

- **Dimmable / analog RGB:** LEDC hardware PWM, 12-bit at ~19.5 kHz (no visible flicker on cameras, no audible whine), gamma-corrected fades. Logic-level N-MOSFET per channel (e.g. AO3400 up to ~4 A, IRLZ44N for more), 100 Ω gate resistor and 10 kΩ pull-down so strips stay dark while the ESP32 boots. Works with 5, 12 or 24 V strips; the strip's own supply goes to the MOSFET drain side.
- **Addressable:** the same GPIO through the 74HCT245, powered via the work-light P-MOSFET switch.
- A second analog RGB strip needs 3 channels: W4, W5 plus GPIO14 as W6. GPIO14 briefly toggles during boot, which the 10 kΩ gate pull-down mostly hides; that is why it is last in line.
- The microphone has its own pins, so it never competes with the work lights.

### Pin map (ESP32-DevKitC, WROOM-32)

| Priority | Function | GPIO | Notes |
|---|---|---|---|
| 1 | Grid data A / B | 16, 17 | via 74HCT245 |
| 1 | Grid power enable | 32 | P-MOSFET high-side switch |
| 2 | Work-light channels W1, W2, W3 | 27, 13, 4 | universal (PWM or addressable) |
| 2 | Work-light addressable power enable | 33 | P-MOSFET, only needed for option 3 |
| 3 | Auxiliary channels W4, W5 | 21, 22 | universal |
| 3 | I2S microphone (INMP441) SCK, WS, SD | 26, 25, 34 | GPIO34 is input-only, fine for data in |
| 3 | Analog microphone (MAX9814) alternative | 35 | ADC1, replaces I2S mic |
| – | Optional microSD (photos on 4 MB boards) | 18, 19, 23, 5 | VSPI |
| – | Button (clear highlight / factory reset) | 0 | on-board BOOT button |
| – | Status LED | 2 | on-board LED on most DevKitC boards |
| – | Spare | 14 (third aux channel), 36, 39 (input-only) | 12 and 15 are strapping pins, left unused |

Final map is confirmed with the schematic in Task 2.

---

## 1. Proposed stack (defaults)

| Area | Default | Why / alternatives |
|---|---|---|
| Build | **PlatformIO in VS Code**, Arduino framework on ESP32 | Best VS Code workflow. Uses the `pioarduino` platform to get Arduino-ESP32 core 3.x (the official PlatformIO platform is stuck on core 2.x). Alternative: ESP-IDF + Arduino as component. |
| MCU | ESP32-DevKitC (WROOM-32), **no PSRAM**, 4/8/16 MB flash | All memory budgets assume ~300 KB usable heap. |
| Flash layout | Custom partition tables. 8 MB and 16 MB: 2 × ~1.9 MB OTA app slots + rest LittleFS (≈4 MB / ≈12 MB). 4 MB fallback: 2 × 1.5 MB OTA + ≈1 MB LittleFS (database only, photos on microSD). | OTA updates over Wi-Fi from day one. |
| Filesystem | **LittleFS** | SPIFFS is deprecated; LittleFS is power-loss safe and supports directories. Optional microSD for large photo collections. |
| Database | **SQLite 3** via `siara-cc/esp32_arduino_sqlite3_lib`, DB file on LittleFS | Small page cache (tuned for no PSRAM). Search with `LIKE` across all text fields; FTS5 only if needed. |
| LEDs | **FastLED** (RMT driver), WS2812B/SK6812 | Supports several parallel outputs, power limiting, great effect ecosystem (MIT). Alternative: NeoPixelBus. |
| Web server | **ESPAsyncWebServer** (maintained `ESP32Async` fork) + WebSocket for live updates | Streams files without buffering in RAM. |
| Web UI | Vanilla JS + small CSS, no framework, gzipped and served from LittleFS | Small and fast on ESP32. Optional npm build step for minify/gzip. |
| Photos | JPG, **resized in the browser** to max 800 px / ~150 KB before upload, stored as `/photos/<part_id>.jpg` | Keeps ESP32 RAM and flash use predictable. |
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
 │  WebServer ── REST API ── PartsService ── SQLite (LittleFS)
 │      │                         │                       │
 │      │                    PhotoStore (LittleFS / SD)   │
 │      │                                                 │
 │  ConfigService (config.json) ── GridMapper (bin x,y → LED idx)
 │                                    │                   │
 │  LedEngine (FastLED, render task) ◄┘                   │
 │    ├─ Locate/highlight layer (top priority)            │
 │    ├─ Effects: fire, sparks, relax, music, demo        │
 │    └─ Work lights W1..W5 (PWM or addressable)          │
 │  AudioService (I2S mic → FFT → bands)                  │
 │  WiFi / mDNS / OTA / BackupService                     │
 └────────────────────────────────────────────────────────┘
```

LED rendering runs in its own FreeRTOS task on core 1; Wi-Fi and the web server stay on core 0 so the LEDs don't stutter during uploads.

## 3. Draft data model

- `parts`: id, part_number, manufacturer, category, description, package, value, quantity, unit, location_bin_id, datasheet_url, notes, photo (filename), created_at, updated_at
- `bins`: id, x, y, label, span_w/span_h (for bins covering several grid cells), led_override (optional custom LED list)
- `categories` (optional lookup table, or free text with autocomplete)
- `meta`: schema_version (for migrations)

One part lives in one bin; one bin can hold many parts. Open question whether a part can be split across bins (see questions).

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
- Source layout (`src/`, `lib/`, `data/` for web files, `test/`), `.clang-format`, GitHub Actions build.
- **Verify:** firmware builds in CI; on your board it boots, prints flash size, chip info and free LittleFS space.

### Task 2: Hardware reference design
- Wiring doc and schematic for the architecture in section 0: LED data pins, 74HCT245 level shifter, P-MOSFET power switches per domain, 5 V supply sizing, injection points, fuse, capacitor and series resistor, mic wiring, optional SD card and button.
- Default pin map that avoids strapping and flash pins.
- Power budget calculator (LEDs × mA) and default brightness/current limit.
- **Verify:** you review the doc against your parts.

### Task 3: Configuration and Wi-Fi
- `config.json` on LittleFS: grid width/height, LEDs per bin (1–3), strip wiring order (row-by-row or serpentine, start corner), LED pin(s), LED type and color order, work-light outputs, brightness and current limit, device name.
- Wi-Fi station + fallback AP with a minimal setup page, mDNS, OTA update.
- **Verify:** fresh board opens an AP, you enter Wi-Fi credentials, it joins your network and answers at `smartbins.local`; firmware updates over the air.

### Task 4: LED driver and grid mapping
- `GridMapper`: bin (x, y) → list of LED indices for any grid size, LEDs-per-bin, serpentine or progressive layout, start corner, multi-cell bins. Unit-tested in `native`.
- `LedEngine` render task with layers (locate highlight above effects), global brightness and power limit.
- Test patterns: walk every bin, row/column sweep, identify bin by number.
- **Verify:** on your strip, the test walk lights bins in the correct physical order.

### Task 5: Workspace lights
- Universal channels W1–W5: per-channel mode (off / white PWM / RGB group / addressable) in config, LEDC PWM with gamma and smooth fades, FastLED for an addressable work strip.
- On/off, brightness, color and white tone, simple per-strip effects; state persisted across reboots.
- **Verify:** each light controllable independently via a temporary REST call.

### Task 6: Web server and REST API foundation
- ESPAsyncWebServer, static gzipped files from LittleFS, JSON REST conventions, error format, WebSocket channel for state pushes.
- Endpoints so far: system info, config get/set, LED test, locate bin, work lights.
- Basic UI shell (navigation, status bar) with no inventory yet.
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
- Add/edit/delete part form with autocomplete for manufacturer and category.
- "Locate" button lights the bin (optionally auto-on when search narrows to one bin); visual grid view showing bin contents and empty bins; click a bin to see its parts.
- **Verify:** end-to-end use from a phone in the lab.

### Task 10: Photos
- Upload JPG (camera or file) → browser resizes/compresses → streamed to flash.
- Thumbnails in results, full view on detail page, delete/replace; storage usage shown.
- Optional microSD as photo store.
- **Verify:** add photos from your phone; flash usage stays within budget.

### Task 11: Backup and restore
- Download the database file safely (DB closed or checkpointed during copy).
- Full backup option: database + photos + config as one archive file.
- Restore with validation (format, schema version) and automatic pre-restore backup; migrations applied on older backups.
- Optional CSV export/import for editing in a spreadsheet.
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
- Hardware page: per-channel mode (off / white PWM / RGB group / addressable) and GPIO assignment for every function, validated against the board (flash, input-only and strapping pins rejected, no double use), applied after a reboot. Holding BOOT during power-up restores the default pin map in case a bad setting leaves the device unreachable.
- Settings page: grid and LED config editor with live preview, Wi-Fi, device name, brightness/current limit, OTA upload.
- Effects page: pick effect, parameters, timer; work-light controls.
- **Verify:** reconfigure the grid size from the browser and the mapping updates without reflashing.

### Task 15: Hardening and release 1.0
- Watchdog, heap monitoring, error pages, input validation, optional web UI password.
- Documentation: build/flash guide, wiring, user guide, screenshots; GitHub release with prebuilt binaries for 8 MB and 16 MB.
- **Verify:** fresh-flash test from the release binaries following only the docs.

### Possible later extras (not planned unless you want them)
Barcode/QR scanning in the UI to find parts, low-stock alerts, MQTT / Home Assistant integration, physical "find" button or rotary encoder, multiple grids/cabinets, printable bin labels.

---

## 5. Clarifying questions

1. ~~Repository~~ answered. 2. ~~Board~~ answered.
3. **LED type:** WS2812B (5 V), SK6812 RGBW, or WS2815 (12 V)? Roughly how many bins total for your first cabinet (e.g. 10 × 6)?
4. ~~Strip wiring~~ decided (section 0).
5. **Bin sizes:** are all bins the same size, or do some bins span several grid cells (wide drawers)?
6. ~~Work lights~~ answered: configurable (white ×3, analog RGB, or addressable).
7. **Part ↔ bin:** can one part be stored in more than one bin, and can one bin hold several different parts? (Default: one bin per part, many parts per bin.)
8. **Fields:** besides manufacturer, category and part number, which fields do you need? Default list is in section 3 (package, value, quantity, datasheet URL, notes...).
9. **Backup scope:** should a backup contain only the database, or database + photos + config as one file? (Default: both options.)
10. **Language:** English-only UI, or should it be translatable (e.g. English/Russian)?
11. **Security:** is the device on a trusted home network only (no login), or do you want an optional password?
12. **Microphone:** do you already have a mic module? (Default: INMP441 I2S.)
13. **Locate behavior:** how long should a found bin stay lit (e.g. 30 s, until dismissed), and in what color/animation?
