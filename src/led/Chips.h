#pragma once

// Per-chipset facts shared by the bin grid and the addressable work strips:
// bit timing, bytes per pixel and current draw for the power estimate.

#include <algorithm>

#include "AppConfig.h"
#include "Color.h"
#include "PowerLimit.h"
#include "RmtStrip.h"

namespace sb {
namespace led {

struct ChipInfo {
  BitTiming timing;
  uint8_t channels;  // bytes per pixel: 3 for RGB, 4 for RGBW
  ChipCurrent current;
};

// Timings are in nanoseconds (T0H, T0L, T1H, T1L), chosen near the middle of
// each datasheet's tolerance window rather than at its nominal value, so that
// clones with slightly different thresholds still decode the bits correctly.
// Currents are per color channel at full value, in tenths of a mA:
//   WS2812/B, SK6812: about 60 mA per pixel at full white (3 x 20 mA), 5 V
//   WS2811 (12 V):    one IC drives a 3-LED segment in series, ~20 mA per segment
//   WS2815 (12 V):    ~25 mA per pixel at full white
inline ChipInfo chipInfo(config::Chipset chip) {
  switch (chip) {
    case config::Chipset::SK6812: return {{300, 900, 600, 600}, 3, {200, 10}};
    case config::Chipset::SK6812_RGBW: return {{300, 900, 600, 600}, 4, {200, 10}};
    case config::Chipset::WS2811: return {{250, 1000, 600, 650}, 3, {67, 10}};
    case config::Chipset::WS2815: return {{300, 900, 750, 400}, 3, {83, 10}};
    case config::Chipset::WS2812:
    case config::Chipset::WS2812B:
    default: return {{400, 850, 800, 450}, 3, {200, 10}};
  }
}

// Turns a color order such as "GRB" into a byte map: output byte i of a pixel
// takes color channel order[i] (0 = R, 1 = G, 2 = B). Invalid letters fall
// back to B, but config validation rejects them before this is ever called.
inline void colorOrderMap(const char *order, uint8_t out[3]) {
  for (int i = 0; i < 3; i++) {
    const char c = order[i];
    out[i] = c == 'R' ? 0 : c == 'G' ? 1 : 2;
  }
}

// Writes one pixel (already scaled and gamma-corrected) into the output buffer
// in the strip's byte order and returns the position after it.
//
// For RGBW chips the part that R, G and B have in common is moved to the white
// LED: (200, 120, 80) becomes RGB (120, 40, 0) + W 80. The white LED makes that
// part of the light more efficiently and with a better tint than mixing it from
// three colors, and the visible color stays the same.
inline uint8_t *encodePixel(Rgb c, uint8_t channels, const uint8_t order[3], uint8_t *o) {
  uint8_t ch[3] = {c.r, c.g, c.b};
  if (channels == 4) {
    const uint8_t w = std::min(c.r, std::min(c.g, c.b));
    for (uint8_t &v : ch) v -= w;
    for (int i = 0; i < 3; i++) *o++ = ch[order[i]];
    *o++ = w;
  } else {
    for (int i = 0; i < 3; i++) *o++ = ch[order[i]];
  }
  return o;
}

}  // namespace led
}  // namespace sb
