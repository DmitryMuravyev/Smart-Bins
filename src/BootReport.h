#pragma once

#include <cstdint>

namespace sb {

// Drives every LED-related output LOW so power switches stay off and no strip
// sees random data while the firmware starts. Call first thing in setup().
void holdOutputsLow();

// Logs chip, flash (with a warning if the build env doesn't match), memory,
// partition table and pin map.
void printBootReport();

// Human-readable byte count from a small rotating buffer (valid for a few calls).
const char *formatBytes(uint64_t bytes);

}  // namespace sb
