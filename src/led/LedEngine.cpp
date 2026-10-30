#include "LedEngine.h"

#include <Arduino.h>

#include <algorithm>
#include <cstring>

#include "Chips.h"
#include "util/MutexLock.h"

namespace sb {
namespace led {

namespace {

const char *TAG = "led";

// Frame period while powered. 16 ms gives ~60 fps, smooth for fades, and a
// 1200-pixel grid split over two data lines needs about 18 ms per line at
// 30 us per pixel, so with two segments the RMT is still mostly idle.
constexpr uint32_t kFrameMs = 16;
// While the strips are switched off there is nothing to send; the task only
// wakes to notice new requests, so a slower tick saves CPU time.
constexpr uint32_t kIdleFrameMs = 40;
// After the power switch closes, the strip's supply rail and the pixel ICs need
// a moment to come up. Data sent earlier can be lost or misread as garbage.
constexpr uint32_t kPowerSettleMs = 30;
// Stay powered briefly after the last pixel went dark, so locating bins one
// after another doesn't click the supply on and off every time.
constexpr uint32_t kPowerOffDelayMs = 1500;
// Test patterns stop by themselves in case nobody comes back to switch them off.
constexpr uint32_t kTestTimeoutMs = 120000;

PulseMode pulseMode(config::LocateMode m) {
  switch (m) {
    case config::LocateMode::SlowPulse: return PulseMode::Slow;
    case config::LocateMode::FastPulse: return PulseMode::Fast;
    default: return PulseMode::Steady;
  }
}

using Lock = MutexLock;

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
  colorOrderMap(g.colorOrder, order_);

  powerPin_ = powerPin;
  if (powerPin_ >= 0) {
    pinMode(powerPin_, OUTPUT);
    digitalWrite(powerPin_, LOW);
  }

  // `pixels` holds what each layer wants to show (full scale, before brightness
  // and gamma); `out` is the encoded byte stream handed to the RMT. They are
  // separate because the RMT reads `out` in the background while the next
  // frame is already being rendered into `pixels`.
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
  // erase() returns the next element, so the iterator only advances when
  // nothing was removed.
  for (auto it = highlights_.begin(); it != highlights_.end();) {
    if (it->x == x && it->y == y) it = highlights_.erase(it);
    else ++it;
  }
  // The list is in start order, so when full the oldest highlight makes room.
  if (highlights_.size() >= kMaxHighlights) highlights_.erase(highlights_.begin());
  highlights_.push_back({x, y, color, mode, now, durationMs, 0});
  return true;
}

void LedEngine::clearLocates() {
  Lock lock(lock_);
  const uint32_t now = millis();
  for (Highlight &h : highlights_) {
    // `| 1` keeps the timestamp non-zero (0 means "still on"); being 1 ms off is invisible.
    if (!h.releasedAt) h.releasedAt = now | 1;
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

// Render loop. Each pass: render the layers under the lock, decide whether the
// strips need power, then encode and send outside the lock so web requests are
// never blocked by the (comparatively slow) encoding.
//
// Power logic: `wantPower` is "something is requested" (a highlight or test,
// even one still fading in from zero), `lit` is "a pixel is visibly on right
// now". Power goes on as soon as something is requested, and off only when
// nothing is requested, nothing is lit and that has lasted kPowerOffDelayMs,
// so fade-outs always finish before the switch opens.
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
    // vTaskDelayUntil keeps a steady frame rate: the time spent rendering is
    // subtracted from the wait instead of being added to it.
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(powered_ ? kFrameMs : kIdleFrameMs));
  }
}

void LedEngine::setPower(bool on) {
  if (!on) {
    // Send one dark frame before cutting power. Otherwise the last frame stays
    // latched in pixels that are still faintly powered through their data
    // inputs. After this the data lines rest low, so no current leaks into an
    // unpowered strip through its data pin ("phantom power").
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

// Adds a colour to a bin's pixels. blendMax rather than overwrite, so two
// overlapping layers (or a bin located twice with different colours while one
// fades out) never darken each other.
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

// Builds this frame's pixels from scratch. Called with the lock held.
// A running test pattern replaces the highlights entirely (it returns early),
// so highlights keep their timers and reappear when the test ends.
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
    // A timed highlight releases itself; from then on it fades out like a manual clear.
    if (!h.releasedAt && h.durationMs && age >= h.durationMs) h.releasedAt = now | 1;
    // Level = fade-in x pulse x fade-out, each 0..255, renormalised after every product.
    uint32_t level = rampUp(age, fade);
    level = level * pulseLevel(h.mode, age) / 255;
    if (h.releasedAt) {
      const uint32_t since = now - h.releasedAt;
      if (since >= fade) {  // fully faded out: drop it (erase returns the next element)
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

// Turns pixels into the byte stream: brightness first, then gamma (gamma must
// come last so dimming stays perceptually even), then colour order. The current
// limit is applied after the estimate, as one factor across both segments,
// because both run from the same supply.
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
    for (const Rgb &px : seg.pixels) o = encodePixel(gamma_(scale(px, level)), channels_, order_, o);
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
