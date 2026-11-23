# Smart Bins hardware

The circuit depends on the board the firmware runs on. Pick yours:

| Board | Hardware guide | Firmware files |
|---|---|---|
| ESP32-DevKitC with an ESP32-WROOM-32 / -32E / -32UE module | [esp32-wroom](esp32-wroom/README.md) | `…-esp32-…` |

The ESP32-WROOM guide also has the schematic of the universal 5/12 V board (section 14), as a preview until its PCB is done. Each board's files sit in its guide's folder, named after the chip, the voltage range and the revision (for example `esp32-wroom/board-5-12v/esp32-wroom-5-12v-v1.0-schematic.pdf`), so a 24 V board or an ESP32-S3 board gets its own folder next to it.

Everything else (the web page, the inventory, the effects, backups) is the same on every board; see the [user guide](../user-guide.md).
