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

Built with [PlatformIO](https://platformio.org/) in VS Code, using the Arduino framework for ESP32. Build instructions will be added with the project skeleton (Task 1).

## License

[MIT](LICENSE) © 2026 Dmitry Muravyev
