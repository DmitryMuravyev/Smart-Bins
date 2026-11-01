#pragma once

#include <cstdint>

namespace sb {

// Drives every LED-related output LOW so power switches stay off and no strip
// sees random data while the firmware starts. Call first thing in setup()
// without arguments: the configuration isn't loaded yet, so this uses the
// default pin map.
void holdOutputsLow();
// Once the configuration is loaded: the same for the configured pin map
// (`gpio` in the order of board::kDefaultPins). A default LED pin that the
// configuration gave to something else is let go again (input), so the
// early hold never fights a device on that line for longer than the boot.
void holdOutputsLow(const int8_t *gpio);

// Logs chip, flash (with a warning if the build env doesn't match), memory,
// partition table and pin map.
void printBootReport();

// Heap that malloc() can actually hand out: byte-addressable RAM. Arduino's
// ESP.getFreeHeap() and friends also count IRAM, which on the classic ESP32
// only allows 32-bit accesses and so can't hold buffers, strings or SQLite's
// pages; with it included the figures read tens of KB higher than what is
// really available. These are what the boot log, the System tab and the
// benchmark report.
uint32_t heapFree();     // free now
uint32_t heapMin();      // lowest free since boot
uint32_t heapLargest();  // largest single block: the biggest malloc() that can succeed

// Human-readable byte count from a small rotating buffer (valid for a few calls).
const char *formatBytes(uint64_t bytes);

}  // namespace sb
