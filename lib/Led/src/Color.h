#pragma once

#include <cmath>
#include <cstdint>

namespace sb {
namespace led {

struct Rgb {
  uint8_t r = 0, g = 0, b = 0;

  constexpr Rgb() = default;
  constexpr Rgb(uint8_t r_, uint8_t g_, uint8_t b_) : r(r_), g(g_), b(b_) {}
  static constexpr Rgb fromHex(uint32_t hex) {
    return Rgb(static_cast<uint8_t>(hex >> 16), static_cast<uint8_t>(hex >> 8), static_cast<uint8_t>(hex));
  }
  constexpr bool isBlack() const { return (r | g | b) == 0; }
  constexpr bool operator==(const Rgb &o) const { return r == o.r && g == o.g && b == o.b; }
};

// Scales a channel by level/255, rounding so that 255 * 255 stays 255 and anything * 0 is 0.
constexpr uint8_t scale8(uint8_t value, uint8_t level) {
  return static_cast<uint8_t>((static_cast<uint16_t>(value) * level + 127) / 255);
}

constexpr Rgb scale(Rgb c, uint8_t level) { return Rgb(scale8(c.r, level), scale8(c.g, level), scale8(c.b, level)); }

// Per-channel maximum: overlapping highlights never darken each other.
constexpr Rgb blendMax(Rgb a, Rgb b) {
  return Rgb(a.r > b.r ? a.r : b.r, a.g > b.g ? a.g : b.g, a.b > b.b ? a.b : b.b);
}

// Gamma correction table so that fades look even to the eye (LEDs are linear, eyes are not).
//
// Plain rounding, so the darkest inputs (1..14) map to 0. An earlier
// version forced every non-zero input up to at least 1, which on 8-bit strips
// was worse: the tail of each fade-out sat at value 1 (clearly visible in
// a dark room), and a dim rainbow broke into blocks where one or two channels
// were 1 and the others 0. With rounding, colours fade out in their own
// proportions and dark parts of a gradient go off evenly.
class Gamma {
 public:
  explicit Gamma(float gamma = 2.2f) {
    for (int i = 0; i < 256; i++) {
      table_[i] = static_cast<uint8_t>(std::lround(std::pow(i / 255.0f, gamma) * 255.0f));
    }
  }
  uint8_t operator()(uint8_t v) const { return table_[v]; }
  Rgb operator()(Rgb c) const { return Rgb(table_[c.r], table_[c.g], table_[c.b]); }

 private:
  uint8_t table_[256];
};

}  // namespace led
}  // namespace sb
