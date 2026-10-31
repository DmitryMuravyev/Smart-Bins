# Smart Bins

An addressable parts library for the home lab, built on the ESP32.

Smart Bins is a grid of plastic storage bins, each one lit by 1–3 addressable LEDs. A web interface keeps an inventory of your components in a SQLite database on the ESP32's flash. Search for a part (for example `STM32`), and the bin where it's stored lights up.

> **Status:** early development. See [docs/PLAN.md](docs/PLAN.md) for the implementation plan and progress.

## Features (planned)

- Configurable bin grid of any size (X × Y), 1–3 pixels per bin, flexible strip wiring
- Inventory in SQLite on the board's flash, with instant global search and sorting by manufacturer, category and part number
- Component photos (JPG)
- Database backup (download) and restore from the browser
- Workspace lighting: an analog RGB strip, or up to 3 addressable or dimmable white strips, plus extra white strips; every change fades smoothly
- Lighting effects: fire, sparks, relax, music-reactive (I2S microphone), demo
- Wi-Fi setup portal, `smartbins.local` via mDNS, over-the-air firmware updates

## Hardware

- ESP32-DevKitC (ESP32-WROOM-32), 4/8/16 MB flash
- Addressable LED strips: WS2812 / WS2812B / SK6812 (5 V) or WS2811 / WS2815 (12/24 V)
- 74HCT245 level shifter, MOSFET power switches, LED power supply (5, 12 or 24 V)
- Optional: INMP441 I2S microphone, microSD card

Schematics, wiring, parts list and power budget: [docs/hardware/README.md](docs/hardware/README.md).

## Development

Built with [PlatformIO](https://platformio.org/) in VS Code, using the Arduino framework for ESP32 (Arduino-ESP32 core 3.x via the [pioarduino](https://github.com/pioarduino/platform-espressif32) platform).

1. Install VS Code and the **PlatformIO IDE** extension (VS Code suggests it when you open this folder).
2. Open the repository folder in VS Code. PlatformIO downloads the toolchain on first build.
3. Choose the environment that matches your board's flash chip in the PlatformIO status bar: `esp32-4mb`, `esp32-8mb` (default) or `esp32-16mb`.
4. Connect the board over USB and run **Upload and Monitor**.

The boot log shows chip, flash, memory and partition information. If the detected flash size differs from the build environment, it prints a warning naming the right one.

**Storage.** Everything the board keeps (`config.json`, the inventory database `inventory.db`, later the photos) lives on one FAT partition behind ESP-IDF's wear levelling. FAT rewrites only the changed 4 KB sectors when the database updates a record, where LittleFS would copy the rest of the file each time. On first boot the partition is formatted.

> **Upgrading from 0.4.x:** version 0.5.0 replaced the LittleFS partition with the FAT one and made the app slots bigger. Flash it once over USB (an over-the-air update can't change the partition table); the board then starts with a default `config.json`, so re-enter your settings or paste your saved file into Settings → Advanced.

### First start and Wi-Fi

1. On first boot (or when your network can't be reached) the board opens a Wi-Fi network named **SmartBins-XXXX**. The on-board LED blinks fast.
2. Join it from a phone or laptop. A setup page opens by itself; if not, browse to `http://192.168.4.1/`.
3. Pick your network, enter the password and press **Connect**. The page shows the address once connected.
4. From then on the board is at **http://smartbins.local/** on your network (or at the IP shown in the serial log).

The BOOT button on the board: hold **5 s** and release to forget the Wi-Fi network (a fixed IP address goes back to automatic too), hold **10 s** and release for a factory reset (Wi-Fi, configuration and web password; the inventory is kept). The LED lights steadily at 5 s and flickers at 10 s.

### Web interface

Open **http://smartbins.local/** (or the board's IP). The pages so far:

- **Lights:** every work light with on/off, level, a colour slider and a white-tone slider (both fit a phone held upright), 12 colour presets and effects. Changes made anywhere (another phone, a script) show up on every open page at once.
- **Grid:** tap a bin to light it, tap it again to turn it off; **Clear all** turns every bin off. Lit bins are shown on every open page. Below the grid: how a located bin lights up (**steady**, **slow pulse** or **fast pulse**, how long it stays on, its colour and the grid brightness; saved at once, no restart) and the test patterns.
- **Settings:** language (English, Español, Русский) and theme (automatic, light, dark), both remembered per browser; the device name, which is also its network address (`NAME.local`); the optional password; Wi-Fi, with an automatic (DHCP) or fixed IP address; and the raw `config.json` editor (saving validates every field, stores the file and restarts the board).
- **System:** firmware, chip, memory, storage and network details, inventory figures (parts and pieces, photos, bins in use, categories, manufacturers, database size), and a restart button.

The UI is built into the firmware (`web/` is gzipped into it at build time by `tools/embed_web.py`), so a firmware update, also over Wi-Fi, always brings the matching UI and there is no separate filesystem upload.

**Password.** Off by default. Once set under Settings, the browser asks for it (user name `admin`) and over-the-air updates need it too (see below). The Wi-Fi setup page stays open on the board's own setup network, so a board that lost its Wi-Fi can always be brought back. Forgot it? A 10 s BOOT factory reset removes it.

### LED tests

With a strip connected to GPIO16 (data A) and the grid power switch on GPIO32, the Grid page has the test patterns:

| Test | What you should see |
|---|---|
| walk bins | one bin at a time, left to right, top to bottom. If the order jumps, fix `layout` / `start` in the config. |
| chain | a single green pixel running along data line A (blue on line B) in wiring order |
| rows / columns | one full row or column at a time |
| colours | the whole grid red, green, blue, white. If red shows as green, change `colorOrder` (e.g. RGB instead of GRB). |

For a single-transistor (inverting) level shifter set `"invertData": true` in the grid section of the config (Settings → Advanced).

### Work lights

Each of the five work channels is set in the `lights` array of the config (W1 first). `mode` is one of:

| Mode | Channels | What it drives |
|---|---|---|
| `off` | any | nothing; the pin stays low |
| `white` | W1–W5 | a plain (single-colour) strip through its N-MOSFET, 12-bit PWM |
| `rgb` | W1+W2+W3 together | an analog RGB strip: W1 = red, W2 = green, W3 = blue |
| `addressable` | W1–W3 | an addressable strip on that channel's data line, powered through the work-strip switch (GPIO33) |

`maxBrightness` (1–255) caps a channel that is too bright for the room. Addressable channels also take `pixels`, `chipset`, `colorOrder`, `invertData` and `currentLimitMa`, as for the grid. Example for option 2 with an addressable strip on W1 and white strips on W4 and W5:

```json
"lights": [
  {"mode": "addressable", "pixels": 120, "chipset": "WS2812B", "colorOrder": "GRB"},
  {"mode": "off"}, {"mode": "off"},
  {"mode": "white"}, {"mode": "white"}
]
```

The Lights page controls them. Every change fades (`fadeMs`), and the state is saved a few seconds after the last change and restored, with a fade-in, after a restart.

### HTTP API

Everything the UI does goes through a small JSON API, handy for scripts and home automation. Endpoints that change something accept POST with a form-encoded body and, for quick tests from the address bar, GET with the same parameters. Errors come back with a 4xx/5xx status and `{"error": "message"}`. With a password set, requests need HTTP digest auth (`curl --digest -u admin:PASSWORD ...`).

| Endpoint | What it does |
|---|---|
| `GET /api/system` | firmware, chip, memory, storage, Wi-Fi, password on/off |
| `POST /api/system/restart` | restarts the board |
| `GET /api/config`, `POST /api/config` | reads / replaces `config.json` (JSON body), then restarts |
| `/api/locate?x=3&y=2` | lights one bin; optional `color=FF0000`, `mode=steady\|slow-pulse\|fast-pulse`, `s=10` (0 = until cleared) |
| `/api/locate/clear[?x=3&y=2]` | fades every located bin out, or just the one given |
| `/api/locate/defaults` | without parameters: reads the defaults; with `color`, `mode`, `s`: changes and saves them |
| `/api/led/test?pattern=walk\|chain\|rows\|columns\|colors\|off` | grid test patterns |
| `/api/led/brightness?value=1..255` | grid brightness, applied and saved |
| `GET /api/led/status` | estimated grid current, frame rate, current limit active, lit bins (`lit: [[x, y], ...]`) and their `version`; `/api/locate` and `/api/locate/clear` answer with the same status |
| `GET /api/lights` | the work lights (`id`, name, type, state) |
| `/api/lights/set?id=0&on=toggle` | changes one light: `on=1\|0\|toggle`, `level=1..255`, `color=FF8800`, `kelvin=2700`, `effect=solid\|breathe\|rainbow` |
| `/api/lights/off` | every light off |
| `POST /api/security` | `password=...` sets the password (4–64 characters), empty removes it |
| `GET /api/parts` | parts, 50 at a time, with the `total` number of matches. All parameters are optional: `q=stm32 lqfp` (every word must appear in some text field, any case, also Cyrillic and accented letters), `category=MCU` and `manufacturer=TI` (exact, any case), `x=3&y=2` (parts in that bin; `x=-1` for parts in no bin), `sort=id\|partNumber\|manufacturer\|category\|description\|package\|value\|quantity\|bin\|updated` with `order=asc\|desc` (empty text values always last), `offset=0&limit=50` (`limit` up to 100) |
| `GET /api/parts/values?field=category` | the distinct values of one text field with their number of `parts`, most used first; `q=res` narrows them like a search word, `limit` up to 500 (default 50). For filter lists and autocomplete |
| `GET /api/part?id=N` | one part: `partNumber`, `manufacturer`, `category`, `description`, `package`, `value`, `unit`, `datasheetUrl`, `notes`, `quantity`, `bin` (`{x, y}` or `null`), `photo`, `created`, `updated` (Unix seconds, 0 before network time) |
| `/api/part/add?partNumber=NE555&quantity=10&x=2&y=1` | adds a part from any of the fields above (a part number or a description is required) |
| `/api/part/update?id=N&...` | changes only the fields given; `x=-1` takes a part out of its bin |
| `/api/part/delete?id=N` | deletes a part |
| `/api/part/adjust?id=N&delta=-1` | quantity up or down, never below 0 |
| `GET /api/bins` | every bin with its `label` and number of `parts` (`outside: true` for a bin left outside after the grid shrank) |
| `/api/bin/label?x=0&y=0&label=Resistors` | names a bin; an empty label goes back to the A1-style name |
| `GET /api/inventory/stats` | the inventory figures shown on the System page |
| `POST /api/inventory/benchmark?n=2000`, then `GET` | inserts n test parts, times typical queries, removes them; the `GET` reports progress and results (the LED strips pause while the parts are written) |
| `ws://smartbins.local/ws` | WebSocket: pushes light changes, grid current and system status as JSON |

### Updating over Wi-Fi

Once the board is on your network, upload new firmware without a cable (8 MB and 16 MB boards; a 4 MB board has room for only one copy of the firmware, so it is updated over USB):

```sh
pio run -e esp32-8mb-ota -t upload    # or esp32-16mb-ota
```

With a web password set, OTA uploads need it too. Add it to the OTA environment in `platformio.ini` (and keep that change out of version control):

```ini
upload_flags = --auth=YOUR_PASSWORD
```

From the command line:

```sh
pio run -e esp32-8mb -t upload -t monitor   # build, flash and open the serial monitor
pio test -e native                          # host-side unit tests, no hardware needed
```

### Project layout

| Path | Contents |
|---|---|
| `src/` | Firmware entry point, web server and API (`src/web/`), LED drivers (`src/led/`) |
| `web/` | Web UI: HTML, CSS, JS and translations (`web/i18n/`), embedded into the firmware at build time |
| `lib/` | Firmware modules; hardware-independent ones are unit-tested on the host |
| `test/` | Unit tests (`pio test -e native`) |
| `partitions/` | Flash partition tables for 4, 8 and 16 MB boards |
| `docs/` | Plan, hardware and user documentation |
| `tools/` | Helper scripts (web UI bundler, power budget calculator) |

## License

[MIT](LICENSE) © 2026 Dmitry Muravyev
