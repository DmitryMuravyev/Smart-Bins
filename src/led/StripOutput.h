#pragma once

// The LED output driver for the board the firmware is built for (SB_BOARD,
// see lib/Board/src/BoardProfile.h). The grid (LedEngine) and the work
// lights (WorkLights) only use StripOutput, so a board with a different
// peripheral, such as the ESP32-S3's parallel LCD output with DMA, adds its
// own driver here without touching the effects or the rendering.
//
// A driver drives one addressable data line and provides:
//
//   bool begin(int gpio, const BitTiming &timing, bool invert);
//       Claims the hardware for `gpio`; false if it can't (logged).
//       `invert` flips the output for an inverting level shifter.
//   void end();
//       Releases it; safe to call twice.
//   static void planStrips(int count);
//       Called once before the first begin() with the number of strips that
//       will be started, so shared hardware can be divided among them.
//   bool write(const uint8_t *data, size_t len);
//       Starts sending one frame of encoded bytes (3 or 4 per pixel). `data`
//       stays unchanged until the next write() or waitDone().
//   bool waitDone(uint32_t timeoutMs);
//       Waits for the frame on the wire to finish.
//   bool ready() const;
//       True between a successful begin() and end().
//
// and implements FlashGuard and effectMillis() (OutputCommon.h). The
// static_asserts below check the signatures, so a new driver that misses one
// fails to compile here rather than somewhere in the effects code.

#include <type_traits>

#include "BoardProfile.h"
#include "OutputCommon.h"

#if SB_BOARD == SB_BOARD_ESP32
#include "RmtStrip.h"
namespace sb {
namespace led {
using StripOutput = RmtStrip;
}  // namespace led
}  // namespace sb
#endif

namespace sb {
namespace led {

static_assert(std::is_same<decltype(&StripOutput::begin), bool (StripOutput::*)(int, const BitTiming &, bool)>::value,
              "StripOutput::begin(int gpio, const BitTiming &, bool invert)");
static_assert(std::is_same<decltype(&StripOutput::end), void (StripOutput::*)()>::value, "StripOutput::end()");
static_assert(std::is_same<decltype(&StripOutput::planStrips), void (*)(int)>::value,
              "static StripOutput::planStrips(int count)");
static_assert(std::is_same<decltype(&StripOutput::write), bool (StripOutput::*)(const uint8_t *, size_t)>::value,
              "StripOutput::write(const uint8_t *, size_t)");
static_assert(std::is_same<decltype(&StripOutput::waitDone), bool (StripOutput::*)(uint32_t)>::value,
              "StripOutput::waitDone(uint32_t timeoutMs)");
static_assert(std::is_same<decltype(&StripOutput::ready), bool (StripOutput::*)() const>::value,
              "StripOutput::ready() const");

}  // namespace led
}  // namespace sb
