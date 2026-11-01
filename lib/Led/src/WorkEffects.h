#pragma once

// Color maths and simple effects for the workspace lights (W1-W5).
//
// The same functions serve both kinds of hardware: an addressable strip asks
// for every pixel (index 0..count-1), while an analog RGB strip is treated as
// a strip with exactly one pixel. That way "rainbow" on an analog strip cycles
// through the colors over time, and on an addressable strip the same rainbow
// is also spread along its length.

#include <cmath>
#include <cstdint>
#include <cstring>

#include "Color.h"
#include "Envelope.h"

namespace sb {
namespace led {

enum class WorkEffect : uint8_t { Solid, Breathe, Rainbow };

constexpr uint32_t kBreathePeriodMs = 6000;  // one slow breath: calm enough for a desk light
constexpr uint8_t kBreatheMinLevel = 40;     // never fully dark, so the light doesn't seem to switch off
constexpr uint32_t kRainbowCycleMs = 12000;  // time for a color to travel once around the wheel

constexpr uint16_t kMinKelvin = 1500;
constexpr uint16_t kMaxKelvin = 10000;

inline const char *toString(WorkEffect e) {
  switch (e) {
    case WorkEffect::Breathe: return "breathe";
    case WorkEffect::Rainbow: return "rainbow";
    default: return "solid";
  }
}

inline bool parseWorkEffect(const char *name, WorkEffect &out) {
  if (!name) return false;
  if (strcmp(name, "solid") == 0) out = WorkEffect::Solid;
  else if (strcmp(name, "breathe") == 0) out = WorkEffect::Breathe;
  else if (strcmp(name, "rainbow") == 0) out = WorkEffect::Rainbow;
  else return false;
  return true;
}

// Linear blend from a (t = 0) to b (t = 255), used for color and effect cross-fades.
constexpr uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t) {
  return static_cast<uint8_t>(a + (static_cast<int>(b) - a) * t / 255);
}

constexpr Rgb lerp(Rgb a, Rgb b, uint8_t t) { return Rgb(lerp8(a.r, b.r, t), lerp8(a.g, b.g, t), lerp8(a.b, b.b, t)); }

// Fully saturated color for a hue of 0..255 (0 = red, 85 = green, 170 = blue).
//
// The wheel is split into six sectors. In each sector one channel is at 255,
// one is 0 and the third ramps up or down, so the brightest channel is always
// at full value. (The simpler three-sector wheel keeps R + G + B constant
// instead, which makes yellow, cyan and magenta look only half as bright.)
inline Rgb hueToRgb(uint8_t hue) {
  const uint16_t pos = static_cast<uint16_t>(hue) * 6;  // 0..1530
  const uint8_t up = static_cast<uint8_t>(pos & 0xFF);   // position inside the sector
  const uint8_t down = static_cast<uint8_t>(255 - up);
  switch (pos >> 8) {  // sector 0..5
    case 0: return Rgb(255, up, 0);    // red -> yellow
    case 1: return Rgb(down, 255, 0);  // yellow -> green
    case 2: return Rgb(0, 255, up);    // green -> cyan
    case 3: return Rgb(0, down, 255);  // cyan -> blue
    case 4: return Rgb(up, 0, 255);    // blue -> magenta
    default: return Rgb(255, 0, down); // magenta -> red
  }
}

// Approximate color of a black-body light source at `kelvin` (1500..10000 K),
// for the "white tone" of RGB and addressable strips: 2700 K is a warm bulb,
// 4000 K neutral, 6500 K daylight.
//
// Curve fit by Tanner Helland (public domain), which is accurate to a few
// percent in this range. RGB LEDs are not true white sources, so the result is
// a good starting point rather than a calibrated color.
inline Rgb kelvinToRgb(uint16_t kelvin) {
  if (kelvin < kMinKelvin) kelvin = kMinKelvin;
  if (kelvin > kMaxKelvin) kelvin = kMaxKelvin;
  const float t = kelvin / 100.0f;
  auto clamp = [](float v) -> uint8_t { return static_cast<uint8_t>(v < 0 ? 0 : v > 255 ? 255 : std::lround(v)); };
  float r, g, b;
  if (t <= 66) {
    r = 255;
    g = 99.4708025861f * std::log(t) - 161.1195681661f;
  } else {
    r = 329.698727446f * std::pow(t - 60, -0.1332047592f);
    g = 288.1221695283f * std::pow(t - 60, -0.0755148492f);
  }
  if (t >= 66) b = 255;
  else if (t <= 19) b = 0;
  else b = 138.5177312231f * std::log(t - 10) - 305.0447927307f;
  return Rgb(clamp(r), clamp(g), clamp(b));
}

// Color of pixel `index` out of `count` for an effect, `elapsedMs` after the
// effect started. Brightness (on/off fades and the user's level) is applied
// afterwards by the caller, so effects only decide the color and its shape.
inline Rgb effectPixel(WorkEffect effect, Rgb base, uint16_t index, uint16_t count, uint32_t elapsedMs) {
  switch (effect) {
    case WorkEffect::Breathe:
      return scale(base, cosineWave(elapsedMs, kBreathePeriodMs, kBreatheMinLevel));
    case WorkEffect::Rainbow: {
      // The time term turns the wheel; the index term spreads one full wheel
      // along the strip. Both are taken modulo 256 by the uint8_t cast, which
      // is exactly the wrap-around a hue needs.
      const uint32_t timeHue = (elapsedMs % kRainbowCycleMs) * 256 / kRainbowCycleMs;
      const uint32_t spread = count > 1 ? static_cast<uint32_t>(index) * 256 / count : 0;
      return hueToRgb(static_cast<uint8_t>(timeHue + spread));
    }
    case WorkEffect::Solid:
    default:
      return base;
  }
}

// True when an effect changes over time, so the output must keep being refreshed.
constexpr bool isAnimated(WorkEffect e) { return e != WorkEffect::Solid; }

// PWM duty for a perceptual level of 0..65535, gamma-corrected to `maxDuty`
// (4095 for 12-bit PWM).
//
// The input is "how bright it should look"; LEDs emit light in proportion to
// duty, while the eye responds roughly to its 2.2th root, so the duty is the
// level raised to the power 2.2. Any non-zero level gives at least one step of
// duty: otherwise the first part of a slow fade-in would sit at 0 and then
// visibly pop on.
inline uint16_t pwmDuty(uint16_t level, uint16_t maxDuty, float gamma = 2.2f) {
  if (level == 0) return 0;
  const float x = level / 65535.0f;
  const long duty = std::lround(std::pow(x, gamma) * maxDuty);
  return static_cast<uint16_t>(duty < 1 ? 1 : duty);
}

}  // namespace led
}  // namespace sb
