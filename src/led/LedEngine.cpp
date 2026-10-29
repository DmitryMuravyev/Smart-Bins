#include "LedEngine.h"

#include <Arduino.h>

#include <algorithm>
#include <cstring>

namespace sb {
namespace led {

namespace {

const char *TAG = "led";

constexpr uint32_t kFrameMs = 16;           // ~60 fps while powered
constexpr uint32_t kIdleFrameMs = 40;       // while the strips are switched off
constexpr uint32_t kPowerSettleMs = 30;     // let the strip supply settle before sending data
constexpr uint32_t kPowerOffDelayMs = 1500; // stay powered briefly after the last pixel goes dark
constexpr uint32_t kTestTimeoutMs = 120000; // test patterns stop by themselves

struct ChipInfo {
  BitTiming timing;
  uint8_t channels;
  ChipCurrent current;
};

ChipInfo chipInfo(config::Chipset chip) {
  switch (chip) {
    case config::Chipset::SK6812: return {{300, 900, 600, 600}, 3, {200, 10}};
    case config::Chipset::SK6812_RGBW: return {{300, 900, 600, 600}, 4, {200, 10}};
    case config::Chipset::WS2811: return {{250, 1000, 600, 650}, 3, {67, 10}};   // 12 V, one IC per segment
    case config::Chipset::WS2815: return {{300, 900, 750, 400}, 3, {83, 10}};    // 12 V
    case config::Chipset::WS2812:
    case config::Chipset::WS2812B:
    default: return {{400, 850, 800, 450}, 3, {200, 10}};
  }
}

PulseMode pulseMode(config::LocateMode m) {
  switch (m) {
    case config::LocateMode::SlowPulse: return PulseMode::Slow;
    case config::LocateMode::FastPulse: return PulseMode::Fast;
    default: return PulseMode::Steady;
  }
}

class Lock {
 public:
  explicit Lock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(m_); }

 private:
  SemaphoreHandle_t m_;
};

const char *const kTestNames[] = {"off", "walk", "chain", "rows", "columns", "colors"};

}  // namespace

const char *toString(TestPattern p) { return kTestNames[static_cast<int>(p)]; }

bool parseTestPattern(const char *name, TestPattern &out) {
  for (int i = 0; i < 6; i++) {
    if (name && strcmp(name, kTestNames[i]) == 0) {
      out = static_cast<TestPattern>(i);
      return true;
    }
  }
  return false;
}

bool LedEngine::begin(const config::AppConfig &cfg, int dataPinA, int dataPinB, int powerPin) {
  cfg_ = cfg;
  const config::GridConfig &g = cfg.grid;

  GridGeometry geo;
  geo.cols = g.cols;
  geo.rows = g.rows;
  geo.pixelsPerBin = g.pixelsPerBin;
  geo.serpentine = g.layout == config::Layout::Serpentine;
  geo.startTop = g.start == config::StartCorner::TopLeft || g.start == config::StartCorner::TopRight;
  geo.startLeft = g.start == config::StartCorner::TopLeft || g.start == config::StartCorner::BottomLeft;
  geo.segments = g.segments;
  geo.splitRow = g.splitRow;
  mapper_ = GridMapper(geo);

  const ChipInfo chip = chipInfo(g.chipset);
  channels_ = chip.channels;
  current_ = chip.current;
  for (int i = 0; i < 3; i++) {
    const char c = g.colorOrder[i];
    order_[i] = c == 'R' ? 0 : c == 'G' ? 1 : 2;
  }

  powerPin_ = powerPin;
  if (powerPin_ >= 0) {
    pinMode(powerPin_, OUTPUT);
    digitalWrite(powerPin_, LOW);
  }

  const int pins[2] = {dataPinA, dataPinB};
  bool ok = true;
  for (uint8_t s = 0; s < mapper_.segmentCount(); s++) {
    Segment &seg = segments_[s];
    seg.pixels.assign(mapper_.segmentLength(s), Rgb());
    seg.out.assign(static_cast<size_t>(mapper_.segmentLength(s)) * channels_, 0);
    ok &= seg.strip.begin(pins[s], chip.timing, g.invertData);
  }

  brightness_ = Fader(g.brightness);
  lock_ = xSemaphoreCreateMutex();
  highlights_.reserve(kMaxHighlights);
  xTaskCreatePinnedToCore(taskEntry, "leds", 4096, this, 2, nullptr, 1);
  ESP_LOGI(TAG, "Grid %ux%u, %u px on A, %u px on B, %s, %s%s", (unsigned)g.cols, (unsigned)g.rows,
           (unsigned)mapper_.segmentLength(0), (unsigned)mapper_.segmentLength(1), config::toString(g.chipset),
           g.colorOrder, g.invertData ? ", inverted data" : "");
  return ok;
}

bool LedEngine::locate(uint8_t x, uint8_t y) {
  const uint32_t duration = static_cast<uint32_t>(cfg_.locate.durationS) * 1000;
  return locate(x, y, Rgb::fromHex(cfg_.locate.color), pulseMode(cfg_.locate.mode), duration);
}

bool LedEngine::locate(uint8_t x, uint8_t y, Rgb color, PulseMode mode, uint32_t durationMs) {
  PixelSpan span;
  if (!mapper_.binSpan(x, y, span)) return false;
  Lock lock(lock_);
  const uint32_t now = millis();
  // Re-locating a bin restarts it instead of stacking a second highlight.
  for (auto it = highlights_.begin(); it != highlights_.end();) {
    if (it->x == x && it->y == y) it = highlights_.erase(it);
    else ++it;
  }
  if (highlights_.size() >= kMaxHighlights) highlights_.erase(highlights_.begin());
  highlights_.push_back({x, y, color, mode, now, durationMs, 0});
  return true;
}

void LedEngine::clearLocates() {
  Lock lock(lock_);
  const uint32_t now = millis();
  for (Highlight &h : highlights_) {
    if (!h.releasedAt) h.releasedAt = now | 1;  // never 0, which means "on"
  }
}

void LedEngine::startTest(TestPattern pattern) {
  Lock lock(lock_);
  test_ = pattern;
  testStart_ = millis();
  ESP_LOGI(TAG, "Test pattern: %s", toString(pattern));
}

void LedEngine::setBrightness(uint8_t level) {
  Lock lock(lock_);
  brightness_.set(level, millis(), cfg_.fadeMs);
}

LedEngine::Status LedEngine::status() {
  Lock lock(lock_);
  return {powered_, static_cast<uint8_t>(highlights_.size()), test_, estimateMa_, limitLevel_, fps_};
}

void LedEngine::taskEntry(void *arg) { static_cast<LedEngine *>(arg)->run(); }

void LedEngine::run() {
  TickType_t wake = xTaskGetTickCount();
  uint32_t frames = 0;
  uint32_t fpsSince = millis();
  for (;;) {
    const uint32_t now = millis();
    bool wantPower;
    {
      Lock lock(lock_);
      renderLayers(now);
      wantPower = test_ != TestPattern::None || !highlights_.empty();
    }
    const bool lit = anyLit();
    if (lit) lastLit_ = now;

    if (wantPower && !powered_) setPower(true);
    if (powered_ && !wantPower && !lit && now - lastLit_ > kPowerOffDelayMs) setPower(false);

    if (powered_ && now - poweredAt_ >= kPowerSettleMs) {
      encodeAndSend(now);
      frames++;
    }
    if (now - fpsSince >= 1000) {
      fps_ = static_cast<uint16_t>(frames * 1000 / (now - fpsSince));
      frames = 0;
      fpsSince = now;
    }
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(powered_ ? kFrameMs : kIdleFrameMs));
  }
}

void LedEngine::setPower(bool on) {
  if (!on) {
    // Send one dark frame, then cut power; the data lines then rest low (no phantom power).
    for (Segment &seg : segments_) {
      if (!seg.strip.ready()) continue;
      std::fill(seg.out.begin(), seg.out.end(), 0);
      seg.strip.write(seg.out.data(), seg.out.size());
      seg.strip.waitDone(50);
    }
  }
  if (powerPin_ >= 0) digitalWrite(powerPin_, on ? HIGH : LOW);
  powered_ = on;
  poweredAt_ = millis();
  ESP_LOGI(TAG, "Grid power %s", on ? "on" : "off");
}

void LedEngine::lightBin(uint8_t x, uint8_t y, Rgb color) {
  PixelSpan span;
  if (!mapper_.binSpan(x, y, span)) return;
  std::vector<Rgb> &px = segments_[span.segment].pixels;
  for (uint8_t i = 0; i < span.count; i++) px[span.first + i] = blendMax(px[span.first + i], color);
}

bool LedEngine::anyLit() const {
  for (const Segment &seg : segments_) {
    for (const Rgb &c : seg.pixels) {
      if (!c.isBlack()) return true;
    }
  }
  return false;
}

void LedEngine::renderLayers(uint32_t now) {
  for (Segment &seg : segments_) std::fill(seg.pixels.begin(), seg.pixels.end(), Rgb());

  if (test_ != TestPattern::None) {
    if (now - testStart_ > kTestTimeoutMs) {
      test_ = TestPattern::None;
    } else {
      renderTest(now);
      return;
    }
  }

  const uint32_t fade = cfg_.fadeMs;
  for (auto it = highlights_.begin(); it != highlights_.end();) {
    Highlight &h = *it;
    const uint32_t age = now - h.start;
    if (!h.releasedAt && h.durationMs && age >= h.durationMs) h.releasedAt = now | 1;
    uint32_t level = rampUp(age, fade);
    level = level * pulseLevel(h.mode, age) / 255;
    if (h.releasedAt) {
      const uint32_t since = now - h.releasedAt;
      if (since >= fade) {
        it = highlights_.erase(it);
        continue;
      }
      level = level * rampDown(since, fade) / 255;
    }
    lightBin(h.x, h.y, scale(h.color, static_cast<uint8_t>(level)));
    ++it;
  }
}

void LedEngine::renderTest(uint32_t now) {
  const GridGeometry &g = mapper_.geometry();
  const uint32_t t = now - testStart_;
  const Rgb white(255, 255, 255);
  switch (test_) {
    case TestPattern::Walk: {  // bins in reading order: left to right, top to bottom
      const uint32_t k = (t / 400) % mapper_.binCount();
      lightBin(k % g.cols, k / g.cols, white);
      break;
    }
    case TestPattern::Chain: {  // one pixel running along each data line, in wiring order
      for (uint8_t s = 0; s < mapper_.segmentCount(); s++) {
        const uint16_t len = mapper_.segmentLength(s);
        if (len) segments_[s].pixels[(t / 40) % len] = s == 0 ? Rgb(0, 255, 0) : Rgb(0, 0, 255);
      }
      break;
    }
    case TestPattern::Rows: {
      const uint8_t row = (t / 600) % g.rows;
      for (uint8_t x = 0; x < g.cols; x++) lightBin(x, row, white);
      break;
    }
    case TestPattern::Columns: {
      const uint8_t col = (t / 600) % g.cols;
      for (uint8_t y = 0; y < g.rows; y++) lightBin(col, y, white);
      break;
    }
    case TestPattern::Colors: {  // red, green, blue, white: checks the colour order
      static const Rgb kColors[] = {Rgb(255, 0, 0), Rgb(0, 255, 0), Rgb(0, 0, 255), white};
      const Rgb c = kColors[(t / 1500) % 4];
      for (Segment &seg : segments_) std::fill(seg.pixels.begin(), seg.pixels.end(), c);
      break;
    }
    case TestPattern::None:
      break;
  }
}

void LedEngine::encodeAndSend(uint32_t now) {
  uint8_t level;
  {
    Lock lock(lock_);
    level = brightness_.value(now);
  }
  uint32_t estimate = 0;
  for (uint8_t s = 0; s < mapper_.segmentCount(); s++) {
    Segment &seg = segments_[s];
    seg.strip.waitDone(50);  // the previous frame must be out before its buffer changes
    uint8_t *o = seg.out.data();
    for (const Rgb &px : seg.pixels) {
      const Rgb c = gamma_(scale(px, level));
      uint8_t ch[3] = {c.r, c.g, c.b};
      if (channels_ == 4) {  // RGBW: move the common part of R, G and B to the white LED
        const uint8_t w = std::min(c.r, std::min(c.g, c.b));
        for (uint8_t &v : ch) v -= w;
        for (int i = 0; i < 3; i++) *o++ = ch[order_[i]];
        *o++ = w;
      } else {
        for (int i = 0; i < 3; i++) *o++ = ch[order_[i]];
      }
    }
    estimate += estimateMilliamps(seg.out.data(), seg.pixels.size(), channels_, current_);
  }

  const uint8_t limit = limitLevel(estimate, cfg_.grid.currentLimitMa);
  for (uint8_t s = 0; s < mapper_.segmentCount(); s++) {
    Segment &seg = segments_[s];
    if (limit < 255) {
      for (uint8_t &b : seg.out) b = scale8(b, limit);
    }
    seg.strip.write(seg.out.data(), seg.out.size());
  }
  estimateMa_ = estimate;
  limitLevel_ = limit;
}

}  // namespace led
}  // namespace sb
