#pragma once

// Renders the bin grid on its own FreeRTOS task (core 1, ~60 fps).
//
// Layers, top first: test pattern (when running), locate highlights. Every
// highlight fades in and out (config fadeMs) and can pulse; brightness changes
// fade too. The grid's power switch turns on before anything is shown and off
// a moment after the last pixel went dark; while off, the data lines rest low.

#include <cstdint>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "AppConfig.h"
#include "Color.h"
#include "Envelope.h"
#include "GridMapper.h"
#include "PowerLimit.h"
#include "RmtStrip.h"

namespace sb {
namespace led {

// The pulse of a highlight for a locate.mode value from the config.
inline PulseMode pulseMode(config::LocateMode m) {
  switch (m) {
    case config::LocateMode::SlowPulse: return PulseMode::Slow;
    case config::LocateMode::FastPulse: return PulseMode::Fast;
    default: return PulseMode::Steady;
  }
}

enum class TestPattern : uint8_t { None, Walk, Chain, Rows, Columns, Colors };

const char *toString(TestPattern p);
bool parseTestPattern(const char *name, TestPattern &out);

class LedEngine {
 public:
  struct Status {
    bool powered;
    uint8_t highlights;
    TestPattern test;
    uint32_t estimateMa;
    uint8_t limitLevel;  // 255 = within the current limit
    uint16_t fps;
  };

  static constexpr uint8_t kMaxHighlights = 32;

  // Sets up the data lines and starts the render task.
  bool begin(const config::AppConfig &cfg, int dataPinA, int dataPinB, int powerPin);

  // Lights a bin with the configured locate colour, mode and duration.
  bool locate(uint8_t x, uint8_t y);
  // durationMs 0 = until cleared.
  bool locate(uint8_t x, uint8_t y, Rgb color, PulseMode mode, uint32_t durationMs);
  void clearLocates();
  // Fades one located bin out; false when it wasn't lit.
  bool clearLocate(uint8_t x, uint8_t y);

  struct Bin {
    uint8_t x, y;
  };
  // Writes the bins that are lit (not yet fading out) into `out` and returns
  // how many there are, at most `max`. `version`, if given, receives version()
  // as of that same moment, so the list and its number always match.
  uint8_t litBins(Bin *out, uint8_t max, uint32_t *version = nullptr);
  // Increases whenever the set of lit bins changes (located, cleared, timed
  // out); the WebSocket pushes the new set when it moves.
  uint32_t version() const { return version_; }
  // Changes the colour, mode and duration used by locate(x, y) from now on.
  void setLocateDefaults(const config::LocateConfig &locate);

  void startTest(TestPattern pattern);
  void setBrightness(uint8_t level);

  Status status();
  const GridMapper &mapper() const { return mapper_; }

 private:
  struct Highlight {
    uint8_t x, y;
    Rgb color;
    PulseMode mode;
    uint32_t start;
    uint32_t durationMs;  // 0 = until cleared
    bool released;        // fading out (cleared, or its duration ran out)
    uint32_t releasedAt;  // when the fade-out started; valid only when `released`
  };

  struct Segment {
    RmtStrip strip;
    std::vector<Rgb> pixels;
    std::vector<uint8_t> out;
  };

  static void taskEntry(void *arg);
  void run();
  void renderLayers(uint32_t now);
  void renderTest(uint32_t now);
  void lightBin(uint8_t x, uint8_t y, Rgb color);
  bool anyLit() const;
  void encodeAndSend(uint32_t now);
  void setPower(bool on);

  config::AppConfig cfg_;
  GridMapper mapper_;
  Segment segments_[2];
  uint8_t channels_ = 3;
  uint8_t order_[3] = {1, 0, 2};  // output byte i takes channel order_[i] (0 = R, 1 = G, 2 = B)
  ChipCurrent current_ = {200, 10};
  Gamma gamma_;
  Fader brightness_;
  int powerPin_ = -1;

  SemaphoreHandle_t lock_ = nullptr;
  std::vector<Highlight> highlights_;
  // Written under the lock, read without it (a 32-bit load is atomic).
  volatile uint32_t version_ = 0;
  TestPattern test_ = TestPattern::None;
  uint32_t testStart_ = 0;

  bool powered_ = false;
  uint32_t poweredAt_ = 0;
  uint32_t lastLit_ = 0;
  uint32_t estimateMa_ = 0;
  uint8_t limitLevel_ = 255;
  uint16_t fps_ = 0;
};

}  // namespace led
}  // namespace sb
