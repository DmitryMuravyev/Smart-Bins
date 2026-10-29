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
    uint32_t releasedAt;  // 0 = still on
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
