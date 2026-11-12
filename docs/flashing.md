# Installing the firmware

There are two ways to get Smart Bins onto a board: write a ready-made release file, or build the firmware yourself. Either way, the first install goes over USB; later updates can go over Wi-Fi.

## 1. Which file

Every [release](https://github.com/DmitryMuravyev/Smart-Bins/releases) has two files per board type:

| Your board | Full image (first install, USB) | Update (Wi-Fi) |
|---|---|---|
| ESP32-WROOM-32E/UE-**N16R2** (16 MB flash, 2 MB PSRAM) | `smartbins-VERSION-esp32-16mb-psram-full.bin` | `smartbins-VERSION-esp32-16mb-psram-update.bin` |
| ESP32-WROOM-32 with 16 MB flash | `smartbins-VERSION-esp32-16mb-full.bin` | `smartbins-VERSION-esp32-16mb-update.bin` |
| ESP32-WROOM-32 with 8 MB flash | `smartbins-VERSION-esp32-8mb-full.bin` | `smartbins-VERSION-esp32-8mb-update.bin` |
| ESP32-WROOM-32 with 4 MB flash | `smartbins-VERSION-esp32-4mb-full.bin` | (USB only: no room for a second copy) |

Not sure about the flash size? The module's name tells it (N4, N8, N16), or `esptool.py flash_id` prints it. If you pick the wrong one, the boot log says so and names the right file.

- The **full image** holds everything: the bootloader, the partition table and the firmware. Write it at address **0x0**.
- The **update** is the firmware alone, for the System page's **Firmware update** or an over-the-air upload. It keeps settings, parts and photos.

## 2. First install over USB

Connect the board with a USB data cable. Most DevKitC boards need no button presses; if the tool can't connect, hold **BOOT**, tap **EN** (reset), then release BOOT.

### From the browser (nothing to install)

1. Open [Espressif's web flasher](https://espressif.github.io/esptool-js/) in Chrome or Edge.
2. Set the baud rate to 921600 and press **Connect**; pick the board's serial port.
3. Optional, for a board that ran other firmware before: press **Erase Flash**.
4. Under **Flash Address** enter `0x0`, choose the full image, and press **Program**.
5. When it finishes, press the board's **EN** button (or unplug and plug it in again).

### From the command line

With Python installed: `pip install esptool`, then

```sh
esptool.py --chip esp32 --baud 921600 write_flash 0x0 smartbins-VERSION-esp32-8mb-full.bin
```

Add `erase_flash` first (`esptool.py --chip esp32 erase_flash`) for a board that ran other firmware.

### What you should see

The serial monitor (115200 baud) shows the boot report: chip, flash, memory, partitions and pin map. Then the board opens its setup Wi-Fi network, and the on-board LED blinks fast. Continue with [First start](user-guide.md#first-start-and-wi-fi).

## 3. Updating

**From the web page** (8 and 16 MB boards): System → **Firmware update** → choose the `-update.bin` file → **Install**. The board checks the file, writes it to its second app slot and restarts. If anything goes wrong (a cut connection, a wrong file), the current firmware keeps running.

**Over USB**: write the full image again as above, without erasing. Settings, parts and photos stay: they live in partitions the full image doesn't cover. (Erasing removes them; take a backup first, see the user guide.)

A 4 MB board has a single app slot and is updated over USB only.

## 4. Building it yourself

Built with [PlatformIO](https://platformio.org/) in VS Code, using the Arduino framework for ESP32 (Arduino-ESP32 core 3.x through the [pioarduino](https://github.com/pioarduino/platform-espressif32) platform).

1. Install VS Code and the **PlatformIO IDE** extension (VS Code suggests it when you open the folder).
2. Clone or download this repository and open its folder in VS Code. PlatformIO downloads the toolchain on the first build.
3. In the PlatformIO status bar, choose the environment for your board: `esp32-4mb`, `esp32-8mb` (default), `esp32-16mb` or `esp32-16mb-psram`. The Build and Upload buttons next to it then use that environment. Or open the PlatformIO sidebar (the ant icon) → Project Tasks → your environment → General.
4. Connect the board and run **Upload and Monitor**. Use USB, not an `-ota` environment, for the first install: only a USB upload writes the bootloader, which sets the flash to 80 MHz QIO.
5. Check the first lines of the boot log: `Smart Bins firmware` with the version you built, and `firmware built for … (env esp32-…)` with your environment. A PSRAM board also shows a `PSRAM: 2.00 MB` line; `PSRAM: none` means a build without PSRAM support was flashed.

If PlatformIO shows "Cannot read properties of undefined (reading 'id')" on Build, it doesn't know which project or environment to use: open the folder that holds `platformio.ini` (not a folder above it), wait until the status bar stops showing "Loading" or "Configuring", pick the environment, and build from the sidebar. **Developer: Reload Window** (Ctrl+Shift+P) clears it if it stays.

From the command line:

```sh
pio run -e esp32-8mb -t upload -t monitor   # build, flash and open the serial monitor
pio test -e native                          # host-side unit tests, no hardware needed
python tools/merge_firmware.py esp32-8mb 1.0.0 release   # the two release files from a build
```

### Updating over Wi-Fi from PlatformIO

```sh
pio run -e esp32-8mb-ota -t upload    # or esp32-16mb-ota, esp32-16mb-psram-ota
```

With a web password set, over-the-air uploads need it too. Add it to the OTA environment in `platformio.ini` (and keep that change out of version control):

```ini
upload_flags = --auth=YOUR_PASSWORD
```

If `smartbins.local` doesn't resolve on your computer, set `upload_port` to the board's IP address.

## 5. Making a release (maintainers)

1. Set the version in `platformio.ini` (`SB_FW_VERSION`) and merge to `main`.
2. Tag that commit and push the tag: `git tag v1.0.0 && git push origin v1.0.0`.
3. CI builds every board type, checks that the tag matches the version, and creates a **draft** release with the files and [the release notes](release-notes.md).
4. Review the draft on GitHub, edit the notes, and publish it.
