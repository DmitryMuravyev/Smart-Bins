#include "LedEngine.h"

#include "PowerSupply.h"

#include <Arduino.h>
#include <esp_random.h>

#include <algorithm>
#include <cstring>

#include "Chips.h"
#include "MemoryProfile.h"
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
    const bool started = seg.strip.begin(pins[s], chip.timing, g.invertData);
    ok &= started;
    // One dark frame right away: a reset (EN button, crash, OTA) doesn't
    // clear the pixels, and a strip powered straight from its supply (no
    // switch on the power pin) would otherwise keep showing its last frame.
    if (started && seg.strip.write(seg.out.data(), seg.out.size())) seg.strip.waitDone(50);
  }

  effects_.configure(g.cols, g.rows);
  // Effect settings changes fade like everything else on the grid.
  effects_.setParamFadeMs(cfg.fadeMs);
  effectFrame_.assign(static_cast<size_t>(g.cols) * g.rows, Rgb());

  brightness_ = Fader(g.brightness);
  lock_ = xSemaphoreCreateMutex();
  highlights_.reserve(kMaxHighlights);
  xTaskCreatePinnedToCore(taskEntry, "leds", mem::kLedStack, this, 2, nullptr, 1);
  ESP_LOGI(TAG, "Grid %ux%u, %u px on A, %u px on B, %s, %s%s", (unsigned)g.cols, (unsigned)g.rows,
           (unsigned)mapper_.segmentLength(0), (unsigned)mapper_.segmentLength(1), config::toString(g.chipset),
           g.colorOrder, g.invertData ? ", inverted data" : "");
  return ok;
}

bool LedEngine::locate(uint8_t x, uint8_t y) {
  // Copied under the lock: setLocateDefaults() may change it from another request.
  config::LocateConfig d;
  {
    Lock lock(lock_);
    d = cfg_.locate;
  }
  return locate(x, y, Rgb::fromHex(d.color), pulseMode(d.mode), static_cast<uint32_t>(d.durationS) * 1000, true,
                true);
}

void LedEngine::setLocateDefaults(const config::LocateConfig &locate) {
  Lock lock(lock_);
  cfg_.locate = locate;
  // The bins lit now follow, so a new color or pattern shows straight away
  // instead of only on the next bin located. Their start time stays, so a
  // pulse keeps its rhythm and a timed bin still goes out on time. Bins
  // already fading out are left to finish.
  const Rgb color = Rgb::fromHex(locate.color);
  const PulseMode mode = pulseMode(locate.mode);
  for (Highlight &h : highlights_) {
    if (h.released) continue;
    if (h.followColor) h.color = color;
    if (h.followMode) h.mode = mode;
  }
}

bool LedEngine::locate(uint8_t x, uint8_t y, Rgb color, PulseMode mode, uint32_t durationMs, bool followColor,
                       bool followMode) {
  PixelSpan span;
  if (!mapper_.binSpan(x, y, span)) return false;
  Lock lock(lock_);
  const uint32_t now = effectMillis();
  // Re-locating a bin replaces its highlight instead of stacking a second one.
  // If the old one is still on (not fading out), the new one takes over its
  // start time, so the bin doesn't dip to dark and fade in again and a pulse
  // keeps its rhythm; the duration is extended to count from now.
  // erase() returns the next element, so the iterator only advances when
  // nothing was removed.
  uint32_t start = now;
  for (auto it = highlights_.begin(); it != highlights_.end();) {
    if (it->x == x && it->y == y) {
      if (!it->released) start = it->start;
      it = highlights_.erase(it);
    } else {
      ++it;
    }
  }
  if (durationMs) durationMs += now - start;
  // The list is in start order, so when full the oldest highlight makes room.
  if (highlights_.size() >= kMaxHighlights) highlights_.erase(highlights_.begin());
  highlights_.push_back({x, y, color, mode, start, durationMs, false, 0, followColor, followMode});
  version_ = version_ + 1;
  return true;
}

void LedEngine::clearLocates() {
  Lock lock(lock_);
  const uint32_t now = effectMillis();
  for (Highlight &h : highlights_) {
    if (h.released) continue;  // already fading out: keep its original start
    h.released = true;
    h.releasedAt = now;
  }
  version_ = version_ + 1;
}

bool LedEngine::clearLocate(uint8_t x, uint8_t y) {
  Lock lock(lock_);
  for (Highlight &h : highlights_) {
    if (h.x != x || h.y != y || h.released) continue;
    h.released = true;
    h.releasedAt = effectMillis();
    version_ = version_ + 1;
    return true;
  }
  return false;
}

uint8_t LedEngine::litBins(Bin *out, uint8_t max, uint32_t *version) {
  Lock lock(lock_);
  // Under the same lock as every change, which bumps version_ after the change.
  if (version) *version = version_;
  uint8_t n = 0;
  for (const Highlight &h : highlights_) {
    if (h.released || n >= max) continue;
    out[n++] = {h.x, h.y};
  }
  return n;
}

bool LedEngine::startEffect(GridEffect effect, const EffectParams &params) {
  Lock lock(lock_);
  if (isMusicEffect(effect) && !microphone_) return false;
  effects_.start(effect, params, effectMillis(), esp_random());
  shownEffect_ = effects_.selected();
  version_ = version_ + 1;
  ESP_LOGI(TAG, "Effect: %s", toString(effect));
  return true;
}

void LedEngine::setEffectParams(const EffectParams &params) {
  Lock lock(lock_);
  effects_.setParams(params, effectMillis());
  version_ = version_ + 1;  // the timer shown in the UI may have changed
}

void LedEngine::setMicrophone(bool present) {
  Lock lock(lock_);
  microphone_ = present;
}

void LedEngine::setAudio(const AudioFrame &frame) {
  // A copy of about 90 bytes under the lock; the render task picks it up
  // with its next frame.
  Lock lock(lock_);
  effects_.setAudio(frame);
}

void LedEngine::startTest(TestPattern pattern) {
  Lock lock(lock_);
  test_ = pattern;
  testStart_ = effectMillis();
  ESP_LOGI(TAG, "Test pattern: %s", toString(pattern));
}

void LedEngine::setBrightness(uint8_t level) {
  Lock lock(lock_);
  brightness_.set(level, effectMillis(), cfg_.fadeMs);
}

void LedEngine::setMaster(bool on) {
  Lock lock(lock_);
  master_.set(on ? 255 : 0, effectMillis(), cfg_.fadeMs);
}

LedEngine::Status LedEngine::status() {
  Lock lock(lock_);
  return {powered_,    static_cast<uint8_t>(highlights_.size()), test_, estimateMa_, limitLevel_, fps_,
          effects_.selected(), effects_.remainingS(effectMillis()), microphone_};
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
  uint32_t fpsSince = effectMillis();
  for (;;) {
    uint32_t now;
    bool wantPower;
    {
      Lock lock(lock_);
      // Read the clock under the lock: locate() and clearLocates() stamp their
      // times under it too, so no stamp is ever later than `now`. Otherwise an
      // unsigned `now - stamp` could wrap around and skip a fade entirely.
      now = effectMillis();
      renderLayers(now);
      // An effect needs power only while the master switch lets it show
      // (its fade-out keeps the strips lit, which `lit` covers).
      wantPower = test_ != TestPattern::None || !highlights_.empty() || (effects_.active() && master_.target() > 0);
    }
    const bool lit = anyLit();
    if (lit) lastLit_ = now;

    // The LED supply first (PS-ON): needed from the first request until the
    // grid's own switch has opened again. The switch closes only once the
    // supply's output is up.
    const bool supplyUp = !supply_ || supply_->want(PowerSupply::Grid, wantPower || powered_);
    if (wantPower && !powered_ && supplyUp) setPower(true);
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
  poweredAt_ = effectMillis();
  ESP_LOGD(TAG, "Grid power %s", on ? "on" : "off");
}

// Adds a color to a bin's pixels. blendMax rather than overwrite, so two
// overlapping layers (or a bin located twice with different colors while one
// fades out) never darken each other.
void LedEngine::lightBin(uint8_t x, uint8_t y, Rgb color) {
  PixelSpan span;
  if (!mapper_.binSpan(x, y, span)) return;
  std::vector<Rgb> &px = segments_[span.segment].pixels;
  for (uint8_t i = 0; i < span.count; i++) px[span.first + i] = blendMax(px[span.first + i], color);
}

// Draws a highlight over what is already in the bin (an effect, or dark):
// at level 255 the bin shows `color` alone, below that the two are blended.
// So a highlight fades in out of the effect and back into it, and a located
// bin is never lost in the fire or the sparks around it.
void LedEngine::overlayBin(uint8_t x, uint8_t y, Rgb color, uint8_t level) {
  PixelSpan span;
  if (!mapper_.binSpan(x, y, span)) return;
  std::vector<Rgb> &px = segments_[span.segment].pixels;
  for (uint8_t i = 0; i < span.count; i++) px[span.first + i] = lerp(px[span.first + i], color, level);
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

  // The grid brightness (the "Bin brightness" slider under "How a bin lights up")
  // is the brightness of located bins and of the test patterns. Effects have
  // their own brightness setting and are not dimmed by it: the two sliders
  // are independent, and turning the bins down doesn't darken the fire.
  const uint8_t binLevel = brightness_.value(now);

  if (test_ != TestPattern::None) {
    if (now - testStart_ > kTestTimeoutMs) {
      test_ = TestPattern::None;
    } else {
      renderTest(now);
      if (binLevel < 255) {
        for (Segment &seg : segments_) {
          for (Rgb &c : seg.pixels) c = scale(c, binLevel);
        }
      }
      return;
    }
  }

  // The effect, one color per bin, under everything else.
  // It keeps running while the master switch has it dark (its timer and
  // its picture go on), so it comes back as it would have been.
  const uint8_t master = master_.value(now);
  if (effects_.render(now, esp_random(), effectFrame_.data()) && master > 0) {
    const uint8_t cols = mapper_.geometry().cols, rows = mapper_.geometry().rows;
    for (uint8_t y = 0; y < rows; y++) {
      for (uint8_t x = 0; x < cols; x++) {
        const Rgb c = effectFrame_[static_cast<size_t>(y) * cols + x];
        lightBin(x, y, master == 255 ? c : scale(c, master));
      }
    }
  }
  // The timer stopped it: tell the pages (the WebSocket pushes on version_).
  if (effects_.selected() != shownEffect_) {
    shownEffect_ = effects_.selected();
    version_ = version_ + 1;
  }

  const uint32_t fade = cfg_.fadeMs;
  for (auto it = highlights_.begin(); it != highlights_.end();) {
    Highlight &h = *it;
    const uint32_t age = now - h.start;
    // A timed highlight releases itself; from then on it fades out like a manual clear.
    // A separate flag rather than "releasedAt != 0": an earlier version used
    // `now | 1` to keep the time non-zero, which lands 1 ms in the future on even
    // milliseconds. `now - releasedAt` then wrapped around to ~4 billion, looked
    // like a finished fade, and the bin went dark at once instead of fading.
    if (!h.released && h.durationMs && age >= h.durationMs) {
      h.released = true;
      h.releasedAt = now;
      version_ = version_ + 1;
    }
    // Level = fade-in x pulse x fade-out, each 0..255, renormalised after every product.
    uint32_t level = rampUp(age, fade);
    level = level * pulseLevel(h.mode, age) / 255;
    if (h.released) {
      const uint32_t since = now - h.releasedAt;
      if (since >= fade) {  // fully faded out: drop it (erase returns the next element)
        it = highlights_.erase(it);
        continue;
      }
      level = level * rampDown(since, fade) / 255;
    }
    overlayBin(h.x, h.y, scale(h.color, binLevel), static_cast<uint8_t>(level));
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
    case TestPattern::Colors: {  // red, green, blue, white: checks the color order
      static const Rgb kColors[] = {Rgb(255, 0, 0), Rgb(0, 255, 0), Rgb(0, 0, 255), white};
      const Rgb c = kColors[(t / 1500) % 4];
      for (Segment &seg : segments_) std::fill(seg.pixels.begin(), seg.pixels.end(), c);
      break;
    }
    case TestPattern::None:
      break;
  }
}

// Turns pixels into the byte stream: gamma (after every brightness, which
// renderLayers() has applied, so dimming stays perceptually even), then color
// order. The current limit is applied after the estimate, as one factor across
// both segments, because both run from the same supply.
void LedEngine::encodeAndSend(uint32_t) {
  uint32_t estimate = 0;
  for (uint8_t s = 0; s < mapper_.segmentCount(); s++) {
    Segment &seg = segments_[s];
    seg.strip.waitDone(50);  // the previous frame must be out before its buffer changes
    uint8_t *o = seg.out.data();
    for (const Rgb &px : seg.pixels) o = encodePixel(gamma_(px), channels_, order_, o);
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
