#include "WorkLights.h"

#include <Arduino.h>
#include <Preferences.h>

#include <algorithm>
#include <cstdio>

#include "util/MutexLock.h"

namespace sb {
namespace led {

namespace {

const char *TAG = "lights";

// 12-bit PWM at 19.5 kHz: 80 MHz APB / 4096 steps is the fastest the LEDC can
// count at this resolution (19.53 kHz). Above 18 kHz the PWM is inaudible
// (no coil or capacitor whine) and too fast to show as flicker on a phone
// camera, while 4096 steps keep the dark end of a fade smooth.
constexpr uint32_t kPwmFrequencyHz = 19500;
constexpr uint8_t kPwmBits = 12;
constexpr uint16_t kPwmMax = (1 << kPwmBits) - 1;

constexpr uint32_t kFrameMs = 16;            // ~60 fps while anything moves
constexpr uint32_t kIdleFrameMs = 50;        // otherwise just watch for changes
constexpr uint32_t kStripRefreshMs = 500;    // resend static strips now and then, to recover from a glitch
constexpr uint32_t kPowerSettleMs = 30;      // as for the grid: let the supply rail come up
constexpr uint32_t kPowerOffDelayMs = 1500;  // and stay on briefly after the last fade-out
// Saving waits until the user has stopped changing things: dragging a slider
// sends many changes, and each NVS write costs flash wear and briefly stalls
// the CPU's flash cache.
constexpr uint32_t kSaveDelayMs = 3000;

const char *kPrefsNamespace = "lights";
const char *kPrefsKey = "state";
constexpr uint8_t kSavedVersion = 1;

// Saved state of one channel slot, all bytes so the layout has no padding and
// is the same on every compiler. `type` makes sure a state saved for, say, an
// RGB light is not applied after the channel was reconfigured as white.
struct SavedSlot {
  uint8_t type, on, level, r, g, b, effect;
};
struct SavedState {
  uint8_t version;
  SavedSlot slots[config::kLightChannels];
};

using Lock = MutexLock;

// "W4" for a single channel, "W1-W3" for the RGB group.
void lightName(config::LightMode type, uint8_t channel, char *out, size_t size) {
  if (type == config::LightMode::Rgb) snprintf(out, size, "W%u-W%u", channel + 1u, channel + 3u);
  else snprintf(out, size, "W%u", channel + 1u);
}

}  // namespace

bool WorkLights::begin(const config::AppConfig &cfg, const int8_t (&pins)[config::kLightChannels], int powerPin) {
  fadeMs_ = cfg.fadeMs;
  powerPin_ = powerPin;
  bool ok = true;

  // Group the five channels into lights. An rgb group always starts at W1 and
  // takes W1-W3 (config validation guarantees all three are rgb), so the loop
  // jumps past W2 and W3 once the group is created.
  for (int ch = 0; ch < config::kLightChannels; ch++) {
    const config::LightConfig &lc = cfg.lights[ch];
    if (lc.mode == config::LightMode::Off) {
      if (pins[ch] >= 0) {
        pinMode(pins[ch], OUTPUT);
        digitalWrite(pins[ch], LOW);
      }
      continue;
    }
    Light &l = lights_[count_];
    l.type = lc.mode;
    l.channel = static_cast<uint8_t>(ch);
    l.maxBrightness = lc.maxBrightness;

    if (lc.mode == config::LightMode::Addressable) {
      l.chip = chipInfo(lc.chipset);
      colorOrderMap(lc.colorOrder, l.order);
      l.pixels = lc.pixels;
      l.currentLimitMa = lc.currentLimitMa;
      l.out.assign(static_cast<size_t>(l.pixels) * l.chip.channels, 0);
      ok &= pins[ch] >= 0 && l.strip.begin(pins[ch], l.chip.timing, lc.invertData);
      hasStrips_ = true;
    } else {
      l.pwmCount = lc.mode == config::LightMode::Rgb ? 3 : 1;
      for (uint8_t k = 0; k < l.pwmCount; k++) {
        const int8_t pin = pins[ch + k];
        l.pwmPins[k] = pin;
        if (pin < 0) continue;
        if (!ledcAttach(pin, kPwmFrequencyHz, kPwmBits)) {
          ESP_LOGE(TAG, "GPIO%d: no PWM channel", pin);
          l.pwmPins[k] = -1;
          ok = false;
          continue;
        }
        ledcWrite(pin, 0);
      }
      if (lc.mode == config::LightMode::Rgb) ch += 2;  // W2 and W3 belong to this light
    }
    count_++;
  }

  if (powerPin_ >= 0) {
    pinMode(powerPin_, OUTPUT);
    digitalWrite(powerPin_, LOW);
  }

  lock_ = xSemaphoreCreateMutex();
  restore();

  // Everything starts dark; lights that were on before the restart fade in.
  const uint32_t now = millis();
  for (uint8_t i = 0; i < count_; i++) {
    Light &l = lights_[i];
    l.level.reset(0);
    l.colorFrom = l.state.color;
    l.prevEffect = l.state.effect;
    l.effectStart = l.prevEffectStart = l.colorStart = now;
    l.level.set(targetLevel(l), now, fadeMs_);
    char name[WorkLights::kNameSize];
    lightName(l.type, l.channel, name, sizeof(name));
    ESP_LOGI(TAG, "Light %u: %s %s%s", i, name, config::toString(l.type), l.state.on ? ", on" : "");
  }

  xTaskCreatePinnedToCore(taskEntry, "lights", 4096, this, 2, nullptr, 1);
  return ok;
}

bool WorkLights::info(uint8_t id, Info &out) {
  if (id >= count_) return false;
  Lock lock(lock_);
  const Light &l = lights_[id];
  out.id = id;
  lightName(l.type, l.channel, out.name, sizeof(out.name));
  out.type = l.type;
  out.pixels = l.pixels;
  out.state = l.state;
  out.estimateMa = l.estimateMa;
  out.currentLimited = l.limitLevel < 255;
  return true;
}

const char *WorkLights::set(uint8_t id, const Change &change) {
  if (id >= count_) return "no such light";
  Lock lock(lock_);
  Light &l = lights_[id];
  // A white strip has a single PWM channel: it can only get brighter or
  // darker, so colours and the rainbow make no sense there. Breathe works,
  // because it only changes the brightness.
  if (l.type == config::LightMode::White) {
    if (change.hasColor) return "a white light has no colour";
    if (change.hasEffect && change.effect == WorkEffect::Rainbow) return "a white light can't show a rainbow";
  }
  applyChange(l, change, millis());
  return nullptr;
}

void WorkLights::allOff() {
  Lock lock(lock_);
  Change off;
  off.power = Change::Power::Off;
  const uint32_t now = millis();
  for (uint8_t i = 0; i < count_; i++) applyChange(lights_[i], off, now);
}

bool WorkLights::stripPower() {
  Lock lock(lock_);
  return powered_;
}

// Called with the lock held. Updates the user state, then starts the fades
// from wherever the output is right now.
void WorkLights::applyChange(Light &l, const Change &c, uint32_t now) {
  State &s = l.state;
  bool on = s.on;
  switch (c.power) {
    case Change::Power::On: on = true; break;
    case Change::Power::Off: on = false; break;
    case Change::Power::Toggle: on = !on; break;
    case Change::Power::Keep:
      // Picking a level, colour or effect means "I want to see it", like on
      // most smart lights, so it also switches the light on.
      if (c.hasLevel || c.hasColor || c.hasEffect) on = true;
      break;
  }
  if (c.hasLevel) {
    if (c.level == 0) on = false;  // keep the old level so "on" restores it
    else s.level = c.level;
  }
  if (c.hasColor && !(c.color == s.color)) {
    l.colorFrom = currentColor(l, now);  // start from the colour shown at this moment
    l.colorStart = now;
    s.color = c.color;
  }
  if (c.hasEffect && c.effect != s.effect) {
    l.prevEffect = s.effect;
    l.prevEffectStart = l.effectStart;
    l.effectStart = now;
    s.effect = c.effect;
  }
  s.on = on;
  l.level.set(targetLevel(l), now, fadeMs_);
  dirtySince_ = now | 1;  // never 0, which means "saved"
  version_ = version_ + 1;
}

// Output level for the current state: level x maxBrightness, scaled from
// 255 x 255 to the 0..65535 range of the fader.
uint16_t WorkLights::targetLevel(const Light &l) const {
  if (!l.state.on) return 0;
  return static_cast<uint16_t>(static_cast<uint32_t>(l.state.level) * l.maxBrightness * 65535u / (255u * 255u));
}

Rgb WorkLights::currentColor(const Light &l, uint32_t now) const {
  return lerp(l.colorFrom, l.state.color, rampUp(now - l.colorStart, fadeMs_));
}

// Colour of one pixel (index 0 for PWM lights), including an effect change
// still in progress: the old effect's pixel blended into the new one.
Rgb WorkLights::effectColor(const Light &l, uint16_t index, uint32_t now) const {
  const uint16_t count = l.pixels ? l.pixels : 1;
  const Rgb base = l.type == config::LightMode::White ? Rgb(255, 255, 255) : currentColor(l, now);
  const Rgb c = effectPixel(l.state.effect, base, index, count, now - l.effectStart);
  const uint8_t t = rampUp(now - l.effectStart, fadeMs_);
  if (t == 255) return c;
  return lerp(effectPixel(l.prevEffect, base, index, count, now - l.prevEffectStart), c, t);
}

// True while the output still changes by itself: a fade, a colour or effect
// cross-fade, or an animated effect on a light that is (or is becoming) visible.
bool WorkLights::moving(const Light &l, uint32_t now) const {
  if (!l.level.settled(now)) return true;
  if (now - l.colorStart < fadeMs_ || now - l.effectStart < fadeMs_) return true;
  return isAnimated(l.state.effect) && l.level.value(now) > 0;
}

void WorkLights::writePwm(Light &l, uint32_t now) {
  const uint16_t level = l.level.value(now);
  const Rgb c = effectColor(l, 0, now);
  const uint8_t channel[3] = {c.r, c.g, c.b};  // white lights use the red value (base is white)
  for (uint8_t k = 0; k < l.pwmCount; k++) {
    if (l.pwmPins[k] < 0) continue;
    // Perceived level of this channel = colour component x light level, then
    // gamma to duty. Doing the gamma on the product (not on each factor) keeps
    // a colour's hue the same at every brightness.
    const uint16_t perceived = static_cast<uint16_t>(static_cast<uint32_t>(channel[k]) * level / 255);
    const uint16_t duty = pwmDuty(perceived, kPwmMax);
    if (duty != l.lastDuty[k]) {
      ledcWrite(l.pwmPins[k], duty);
      l.lastDuty[k] = duty;
    }
  }
}

void WorkLights::renderStrip(Light &l, uint32_t now) {
  if (!l.strip.ready()) return;
  // Divide by 257 (not 256) so that 65535 maps to exactly 255.
  const uint8_t level = static_cast<uint8_t>(l.level.value(now) / 257);
  l.strip.waitDone(50);  // the previous frame must be out before `out` changes
  uint8_t *o = l.out.data();
  // Solid and breathe give every pixel the same colour, so it is computed once
  // instead of once per pixel (breathe costs a cosine).
  const bool uniform = l.state.effect != WorkEffect::Rainbow && l.prevEffect != WorkEffect::Rainbow;
  const Rgb same = effectColor(l, 0, now);
  for (uint16_t i = 0; i < l.pixels; i++) {
    const Rgb c = uniform ? same : effectColor(l, i, now);
    o = encodePixel(gamma_(scale(c, level)), l.chip.channels, l.order, o);
  }
  l.estimateMa = estimateMilliamps(l.out.data(), l.pixels, l.chip.channels, l.chip.current);
  l.limitLevel = limitLevel(l.estimateMa, l.currentLimitMa);
  if (l.limitLevel < 255) {
    for (uint8_t &b : l.out) b = scale8(b, l.limitLevel);
  }
  l.strip.write(l.out.data(), l.out.size());
  l.lastSent = now;
}

void WorkLights::setStripPower(bool on) {
  if (!on) {
    // Dark frame first, as for the grid, so nothing stays latched and the data
    // lines rest low while the strips are unpowered.
    for (uint8_t i = 0; i < count_; i++) {
      Light &l = lights_[i];
      if (l.type != config::LightMode::Addressable || !l.strip.ready()) continue;
      std::fill(l.out.begin(), l.out.end(), 0);
      l.strip.write(l.out.data(), l.out.size());
      l.strip.waitDone(50);
    }
  }
  if (powerPin_ >= 0) digitalWrite(powerPin_, on ? HIGH : LOW);
  powered_ = on;
  poweredAt_ = millis();
  ESP_LOGI(TAG, "Work strip power %s", on ? "on" : "off");
}

void WorkLights::taskEntry(void *arg) { static_cast<WorkLights *>(arg)->run(); }

// Output loop. The work happens under the lock, which is fine here: a frame of
// PWM writes takes microseconds and even a 1000-pixel strip encodes in about a
// millisecond, so a web request waits at most that long.
//
// Strip power follows the same rule as the grid: on as soon as any strip is
// wanted, off once all strips are dark and have been for kPowerOffDelayMs.
void WorkLights::run() {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    const uint32_t now = millis();
    bool anyMoving = false;
    bool saveNow = false;
    {
      Lock lock(lock_);
      bool wantStrips = false;
      bool stripsLit = false;
      for (uint8_t i = 0; i < count_; i++) {
        Light &l = lights_[i];
        const bool m = moving(l, now);
        anyMoving |= m;
        if (l.type != config::LightMode::Addressable) {
          writePwm(l, now);
          continue;
        }
        wantStrips |= l.level.target() > 0;
        stripsLit |= l.level.value(now) > 0;
        // Send while moving, plus one more frame after the movement stops:
        // `moving` turns false on the first pass where the fade has ended, and
        // the frame sent on the pass before still shows the last in-between
        // step (the faint tail of a fade-out). `finalFrame` stays set until
        // that closing frame has really gone out, even if the strip was
        // waiting for power at that moment.
        // Also resend a still picture every kStripRefreshMs: a pixel that caught
        // a glitch (noise on a long data wire) would otherwise keep the wrong
        // colour until the next change.
        if (m) l.finalFrame = true;
        if (powered_ && now - poweredAt_ >= kPowerSettleMs &&
            (m || l.finalFrame || now - l.lastSent >= kStripRefreshMs)) {
          renderStrip(l, now);
          if (!m) l.finalFrame = false;
        }
      }
      if (stripsLit) lastLit_ = now;
      if (hasStrips_) {
        if ((wantStrips || stripsLit) && !powered_) setStripPower(true);
        if (powered_ && !wantStrips && !stripsLit && now - lastLit_ > kPowerOffDelayMs) setStripPower(false);
      }
      saveNow = dirtySince_ && now - dirtySince_ >= kSaveDelayMs;
      if (saveNow) dirtySince_ = 0;
    }
    // Outside the lock: an NVS write can take tens of milliseconds.
    if (saveNow) save();
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(anyMoving || powered_ ? kFrameMs : kIdleFrameMs));
  }
}

// Loads the saved state. Anything unexpected (no saved state yet, an older
// layout, a slot saved for another light type) silently keeps the defaults.
void WorkLights::restore() {
  Preferences prefs;
  // Read-write so a missing namespace is created instead of logging an error on first boot.
  if (!prefs.begin(kPrefsNamespace, false)) return;
  SavedState saved{};
  // isKey first: reading a missing key works but logs an error on first boot.
  // The length check rejects a blob saved by a firmware with another layout.
  size_t len = 0;
  if (prefs.isKey(kPrefsKey) && prefs.getBytesLength(kPrefsKey) == sizeof(saved)) {
    len = prefs.getBytes(kPrefsKey, &saved, sizeof(saved));
  }
  prefs.end();
  if (len != sizeof(saved) || saved.version != kSavedVersion) return;
  for (uint8_t i = 0; i < count_; i++) {
    Light &l = lights_[i];
    const SavedSlot &s = saved.slots[l.channel];
    if (s.type != static_cast<uint8_t>(l.type)) continue;
    l.state.on = s.on != 0;
    l.state.level = s.level ? s.level : 255;
    l.state.color = Rgb(s.r, s.g, s.b);
    l.state.effect = s.effect <= static_cast<uint8_t>(WorkEffect::Rainbow) ? static_cast<WorkEffect>(s.effect)
                                                                           : WorkEffect::Solid;
  }
}

void WorkLights::save() {
  SavedState saved{};
  saved.version = kSavedVersion;
  {
    Lock lock(lock_);
    for (uint8_t i = 0; i < count_; i++) {
      const Light &l = lights_[i];
      const State &s = l.state;
      saved.slots[l.channel] = {static_cast<uint8_t>(l.type), s.on, s.level, s.color.r, s.color.g, s.color.b,
                                static_cast<uint8_t>(s.effect)};
    }
  }
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) return;
  prefs.putBytes(kPrefsKey, &saved, sizeof(saved));
  prefs.end();
  ESP_LOGD(TAG, "State saved");
}

}  // namespace led
}  // namespace sb
