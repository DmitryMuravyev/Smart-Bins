#pragma once

// Renders the bin grid on its own FreeRTOS task (core 1, ~60 fps).
//
// Layers, top first: test pattern (when running, replaces the rest), locate
// highlights, effects (fire, sparks, relax, demo, and with a microphone
// spectrum, beat and pulse; see GridEffects.h). Every
// highlight fades in and out (config fadeMs) and can pulse, and is drawn over
// the effect: a located bin always shows its highlight color. Brightness
// changes fade too. The grid's power switch turns on before anything is shown and off
// a moment after the last pixel went dark; while off, the data lines rest low.

#include <cstdint>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "AppConfig.h"
#include "Color.h"
#include "Envelope.h"
#include "GridEffects.h"
#include "GridMapper.h"
#include "PowerLimit.h"
#include "RmtStrip.h"

namespace sb {
class PowerSupply;
namespace led {

// The pulse of a highlight for a locate.mode value from the config.
inline PulseMode pulseMode(config::LocateMode m) {
  switch (m) {
    case config::LocateMode::SlowPulse: return PulseMode::Slow;
    case config::LocateMode::FastPulse: return PulseMode::Fast;
    default: return PulseMode::Steady;
  }
}

// One effect's settings as kept in config.json (effects.<name>.*).
inline EffectParams effectParams(const config::EffectSettings &c) {
  EffectParams p;
  p.speed = c.speed;
  p.intensity = c.intensity;
  p.palette = static_cast<Palette>(c.palette);
  p.brightness = c.brightness;
  p.timerMin = c.timerMin;
  return p;
}

// White: every pixel at full white, past the bin brightness and the
// current limit, to measure what the grid really draws (config pixelMa).
enum class TestPattern : uint8_t { None, Walk, Chain, Rows, Columns, Colors, White };

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
    GridEffect effect;    // what was started (demo stays demo while it cycles)
    uint32_t remainingS;  // until the effect's timer stops it; 0 = no timer
    bool microphone;      // a microphone was found: the music effects can run
  };

  static constexpr uint8_t kMaxHighlights = 32;

  // Sets up the data lines and starts the render task.
  bool begin(const config::AppConfig &cfg, int dataPinA, int dataPinB, int powerPin);

  // Lights a bin with the configured locate color, mode and duration.
  bool locate(uint8_t x, uint8_t y);
  // durationMs 0 = until cleared. followColor / followMode: the color or the
  // pattern came from the locate defaults, and changes with them while the
  // bin is lit (see setLocateDefaults()).
  bool locate(uint8_t x, uint8_t y, Rgb color, PulseMode mode, uint32_t durationMs, bool followColor = false,
              bool followMode = false);
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
  // Changes the color, mode and duration used by locate(x, y) from now on.
  // Bins lit now take the new color and pattern too (unless they were lit
  // with their own); each keeps the time it was lit for.
  void setLocateDefaults(const config::LocateConfig &locate);

  // Starts an effect under the highlights with its settings, fading over
  // from whatever showed before (Off fades the effect out; `params` don't
  // matter then). Its timer, if any, starts now. False (and nothing
  // changes) for a music effect without a microphone.
  bool startEffect(GridEffect effect, const EffectParams &params);
  // Changes the running effect's settings: speed, intensity, palette and
  // brightness fade over to the new values (fadeMs); a changed timer counts
  // from now.
  void setEffectParams(const EffectParams &params);

  // From the microphone task: a microphone was found, and the latest sound
  // analysis (about 62 times a second) for the music effects.
  void setMicrophone(bool present);
  void setAudio(const AudioFrame &frame);

  void startTest(TestPattern pattern);
  // Brightness of located bins and test patterns (grid.brightness), with a
  // fade. Effects have their own brightness and don't follow this one.
  void setBrightness(uint8_t level);
  // The LED supply's PS-ON control (see PowerSupply.h); nullptr: none. Set
  // before begin().
  void setSupply(PowerSupply *supply) { supply_ = supply; }
  // The motion sensor's master switch (see MotionSensor.h): off fades the
  // running effect to dark without stopping or changing it, on brings it
  // back. Located bins and test patterns are not affected: they are asked
  // for by someone at a page, who is there.
  void setMaster(bool on);

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
    bool followColor;     // the color is the locate default's (see setLocateDefaults())
    bool followMode;      // the pattern is the locate default's
    // A color change cross-fades over fadeMs, as the work lights' colors do:
    // the bin shows colorFrom blended into color, starting at colorAt.
    Rgb colorFrom;
    uint32_t colorAt;
  };
  // The color a highlight shows at `now` (mid cross-fade, or `color`).
  Rgb highlightColor(const Highlight &h, uint32_t now) const;
  // Gives a lit highlight a new color, cross-fading from what it shows now.
  void recolor(Highlight &h, Rgb color, uint32_t now);

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
  void overlayBin(uint8_t x, uint8_t y, Rgb color, uint8_t level);
  bool anyLit() const;
  void encodeAndSend(uint32_t now);
  void setPower(bool on, uint32_t now);

  config::AppConfig cfg_;
  GridMapper mapper_;
  Segment segments_[2];
  uint8_t channels_ = 3;
  uint8_t order_[3] = {1, 0, 2};  // output byte i takes channel order_[i] (0 = R, 1 = G, 2 = B)
  ChipCurrent current_ = {200, 10};
  Gamma gamma_;
  Fader brightness_;
  Fader master_{255};  // the effect layer's level under the master switch
  PowerSupply *supply_ = nullptr;
  int powerPin_ = -1;

  SemaphoreHandle_t lock_ = nullptr;
  std::vector<Highlight> highlights_;
  // Written under the lock, read without it (a 32-bit load is atomic).
  volatile uint32_t version_ = 0;
  EffectPlayer effects_;
  std::vector<Rgb> effectFrame_;  // one color per bin, row by row
  GridEffect shownEffect_ = GridEffect::Off;  // last effect reported through version_
  bool microphone_ = false;
  TestPattern test_ = TestPattern::None;
  uint32_t testStart_ = 0;
  // The frame renderLayers() just drew is the white test: encodeAndSend()
  // sends it without the current limit. Set and read on the LED task, so the
  // two always agree about one frame even if the test changes in between.
  bool uncapped_ = false;

  bool powered_ = false;
  uint32_t poweredAt_ = 0;
  uint32_t lastLit_ = 0;
  uint32_t estimateMa_ = 0;
  uint8_t limitLevel_ = 255;
  uint16_t fps_ = 0;
};

}  // namespace led
}  // namespace sb
