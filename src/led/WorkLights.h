#pragma once

// Workspace lights W1-W5: white PWM strips, an analog RGB strip on W1-W3, and
// addressable work strips, all with smooth fades on every change.
//
// Channels are grouped into "lights", the things a user switches:
//   - a white channel is one light;
//   - W1, W2 and W3 in rgb mode together form one RGB light (R, G, B);
//   - an addressable channel is one light driving a whole strip.
// A light is identified by its position in that list (id 0, 1, ...) and named
// after its channels ("W1", "W1-W3").
//
// Every light has a user state (on, level, color, effect) that is saved to NVS
// a few seconds after the last change and restored on boot, with a fade-in.
// Output runs on its own task, like the grid: PWM duty is written only when it
// changes, addressable strips are refreshed while anything moves.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "AppConfig.h"
#include "Chips.h"
#include "Color.h"
#include "Envelope.h"
#include "RmtStrip.h"
#include "WorkEffects.h"

namespace sb {
class PowerSupply;
namespace led {

class WorkLights {
 public:
  struct State {
    bool on = false;
    uint8_t level = 255;  // 1..255, relative to the channel's maxBrightness
    Rgb color = Rgb(255, 255, 255);
    WorkEffect effect = WorkEffect::Solid;
  };

  // A requested change. Fields that are not set keep their current value.
  struct Change {
    enum class Power : uint8_t { Keep, On, Off, Toggle };
    Power power = Power::Keep;
    bool hasLevel = false;
    uint8_t level = 0;  // 0 switches the light off and keeps its previous level
    bool hasColor = false;
    Rgb color;
    bool hasEffect = false;
    WorkEffect effect = WorkEffect::Solid;
  };

  // Longest name is "W1-W3" (6 bytes with the terminator). The compiler can't
  // see that the channel is at most 4, so it sizes the warning check for any
  // uint8_t ("W256-W258"); 12 bytes fits that and keeps -Wformat-truncation quiet.
  static constexpr size_t kNameSize = 12;

  struct Info {
    uint8_t id;
    char name[kNameSize];
    config::LightMode type;
    uint16_t pixels;  // addressable only
    State state;
    uint32_t estimateMa;  // addressable only, before the current limit
    bool currentLimited;
    bool whiteTest;       // the full-white test is running (see whiteTest())
  };

  static constexpr uint8_t kMaxLights = config::kLightChannels;

  // `pins` are the GPIOs of W1-W5 (kNoPin to leave a channel unused);
  // `powerPin` switches the supply of the addressable work strips.
  bool begin(const config::AppConfig &cfg, const int8_t (&pins)[config::kLightChannels], int powerPin);

  uint8_t count() const { return count_; }
  bool info(uint8_t id, Info &out);
  // Returns nullptr on success, otherwise a message for the user.
  const char *set(uint8_t id, const Change &change);
  void allOff();
  // Full white on the addressable strip of `channel` (0 = W1), past the
  // light's level, the maximum brightness and the current limit, to measure
  // what the strip really draws (config pixelMa). Stops by itself after
  // kWhiteTestMs, or with on = false. The light's own state is untouched and
  // shows again afterwards. Returns nullptr on success, otherwise a message.
  const char *whiteTest(uint8_t channel, bool on);
  static constexpr uint32_t kWhiteTestMs = 120000;
  bool stripPower();
  // The LED supply's PS-ON control (see PowerSupply.h); nullptr: none. Set
  // before begin().
  void setSupply(PowerSupply *supply) { supply_ = supply; }
  // The motion sensor's master switch (see MotionSensor.h). Off fades every
  // light to dark; on fades back the ones that are on. The lights' own
  // states (on/off, level, color, effect) are not touched, so the Lights
  // page keeps showing (and saving) what the user set.
  void setMaster(bool on);
  bool master();
  // Increases with every state change; the WebSocket pushes the lights when it moves.
  uint32_t version() const { return version_; }
  // Increases whenever an addressable light's current estimate (or whether
  // it is being limited) changes. The estimate follows the picture, which
  // keeps changing after the request that started it (a fade runs for the
  // fade time, rainbow and breathe never stop), so the WebSocket polls this
  // too and pushes the lights now and then while it moves.
  uint32_t powerVersion() const { return powerVersion_; }

 private:
  struct Light {
    config::LightMode type = config::LightMode::Off;
    uint8_t channel = 0;  // first channel (0 = W1); also the key of its saved state
    uint8_t maxBrightness = 255;
    State state;

    // Output envelope, 0..65535: the user's level scaled by maxBrightness while
    // on, 0 while off. 16 bits so slow PWM fades have no visible steps.
    Fader16 level;
    // Color cross-fade from `colorFrom` to state.color, started at colorStart.
    Rgb colorFrom;
    uint32_t colorStart = 0;
    // Effect cross-fade: the previous effect keeps running (from its own start
    // time) and is blended into the new one over the fade time.
    WorkEffect prevEffect = WorkEffect::Solid;
    uint32_t prevEffectStart = 0;
    uint32_t effectStart = 0;

    // PWM channels (white: 1, rgb: 3). lastDuty avoids rewriting an unchanged
    // duty 60 times a second; 0xFFFF forces the first write.
    uint8_t pwmCount = 0;
    int8_t pwmPins[3] = {-1, -1, -1};
    uint16_t lastDuty[3] = {0xFFFF, 0xFFFF, 0xFFFF};

    // Addressable strip.
    RmtStrip strip;
    std::vector<uint8_t> out;  // encoded bytes; read by the RMT in the background
    ChipInfo chip{};
    uint8_t order[3] = {1, 0, 2};
    uint16_t pixels = 0;
    uint16_t currentLimitMa = 0;
    uint32_t estimateMa = 0;
    uint8_t limitLevel = 255;
    uint32_t lastSent = 0;
    bool finalFrame = false;  // a fade just ended and its last frame is not sent yet
    bool whiteTest = false;   // see whiteTest()
    uint32_t whiteTestStart = 0;
    Fader white;              // the white test's level over the picture, 0..255
  };

  static void taskEntry(void *arg);
  void run();
  uint16_t targetLevel(const Light &l) const;
  Rgb currentColor(const Light &l, uint32_t now) const;
  Rgb effectColor(const Light &l, uint16_t index, uint32_t now) const;
  bool moving(const Light &l, uint32_t now) const;
  void applyChange(Light &l, const Change &c, uint32_t now);
  void writePwm(Light &l, uint32_t now);
  void renderStrip(Light &l, uint32_t now);
  void setStripPower(bool on);
  void restore();
  void save();

  uint16_t fadeMs_ = 400;
  Light lights_[kMaxLights];
  uint8_t count_ = 0;
  Gamma gamma_;

  int powerPin_ = -1;
  PowerSupply *supply_ = nullptr;
  bool master_ = true;
  bool hasStrips_ = false;
  bool powered_ = false;
  uint32_t poweredAt_ = 0;
  uint32_t lastLit_ = 0;

  // Time of the last unsaved change, 0 when everything is saved.
  uint32_t dirtySince_ = 0;
  // Written under the lock, read without it: a 32-bit load is atomic on the ESP32,
  // and a reader that sees the old value just picks up the change one poll later.
  volatile uint32_t version_ = 0;
  volatile uint32_t powerVersion_ = 0;  // the same, for the current estimates
  SemaphoreHandle_t lock_ = nullptr;
};

}  // namespace led
}  // namespace sb
