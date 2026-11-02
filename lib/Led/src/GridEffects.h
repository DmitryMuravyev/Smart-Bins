#pragma once

// Lighting effects for the bin grid: fire, sparks, relax and demo (Task 12),
// and the music effects spectrum, beat and pulse (Task 13), which draw from
// the microphone through AudioAnalyzer.
//
// Effects work on bins, not pixels: the grid is a few dozen bins (10 x 6 on
// Dmitry's build), and every pixel of a bin shows the bin's color. That
// keeps the pictures readable on such a coarse "screen" and the arithmetic
// small enough to run 60 times a second next to everything else.
//
// The pieces, from the bottom up:
//   Palette        a color gradient sampled by a 0..255 index
//   EffectParams   speed, intensity, palette, brightness, timer
//   EffectState    one running effect: its simulation (fire's heat, the
//                  sparks) and its random numbers
//   EffectPlayer   what the LED engine talks to: start/stop, cross-fades
//                  between effects, the demo cycle and the stop timer
//
// Nothing here touches hardware or reads a clock: every call takes the time
// in milliseconds from the caller (the engine passes its effect clock, which
// stands still during flash access). The host unit tests drive it the same way.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "AudioAnalyzer.h"
#include "Color.h"
#include "Envelope.h"
#include "WorkEffects.h"

namespace sb {
namespace led {

enum class GridEffect : uint8_t { Off, Fire, Sparks, Relax, Demo, Spectrum, Beat, Pulse };
constexpr int kGridEffectCount = 8;

// The effects that follow the microphone; they can only start when one is fitted.
inline bool isMusicEffect(GridEffect e) {
  return e == GridEffect::Spectrum || e == GridEffect::Beat || e == GridEffect::Pulse;
}

inline const char *toString(GridEffect e) {
  static const char *const kNames[] = {"off", "fire", "sparks", "relax", "demo", "spectrum", "beat", "pulse"};
  const int i = static_cast<int>(e);
  return i < kGridEffectCount ? kNames[i] : "off";
}

inline bool parseGridEffect(const char *name, GridEffect &out) {
  for (int i = 0; name && i < kGridEffectCount; i++) {
    if (strcmp(name, toString(static_cast<GridEffect>(i))) == 0) {
      out = static_cast<GridEffect>(i);
      return true;
    }
  }
  return false;
}

// ---- Palettes ----
//
// A palette is a short list of color stops spread evenly over the index
// range 0..255; colors in between are blended. "auto" means the effect's
// own palette (fire and pulse: heat, sparks and beat: party, relax: ocean,
// spectrum: rainbow).
enum class Palette : uint8_t { Auto, Heat, Ocean, Forest, Lava, Rainbow, Party };
constexpr int kPaletteCount = 7;

// The names used by the API and config.json (effects.palette), in enum order.
inline const char *const *paletteNames() {
  static const char *const kNames[] = {"auto", "heat", "ocean", "forest", "lava", "rainbow", "party"};
  return kNames;
}

inline const char *toString(Palette p) {
  const int i = static_cast<int>(p);
  return paletteNames()[i < kPaletteCount ? i : 0];
}

inline bool parsePalette(const char *name, Palette &out) {
  for (int i = 0; name && i < kPaletteCount; i++) {
    if (strcmp(name, paletteNames()[i]) == 0) {
      out = static_cast<Palette>(i);
      return true;
    }
  }
  return false;
}

struct PaletteStops {
  const Rgb *stops;
  uint8_t count;
  bool wraps;  // the last stop blends back into the first (rainbow, party)
};

inline PaletteStops paletteStops(Palette p) {
  // Heat: black body from dark red through orange and yellow to white, the
  // usual fire colors. Starting at black lets cold cells go dark.
  static const Rgb kHeat[] = {Rgb(0, 0, 0), Rgb(90, 0, 0), Rgb(200, 20, 0), Rgb(255, 80, 0), Rgb(255, 160, 10),
                              Rgb(255, 230, 80), Rgb(255, 255, 200)};
  // Ocean and forest stay well clear of black: relax shows them as calm
  // fields of color, and after gamma correction dark stops would read as holes.
  static const Rgb kOcean[] = {Rgb(0, 40, 120), Rgb(0, 90, 180), Rgb(0, 160, 200), Rgb(40, 200, 190),
                               Rgb(80, 110, 230), Rgb(20, 60, 170)};
  static const Rgb kForest[] = {Rgb(10, 80, 10), Rgb(40, 140, 20), Rgb(110, 180, 30), Rgb(40, 160, 70),
                                Rgb(160, 190, 40), Rgb(20, 110, 40)};
  static const Rgb kLava[] = {Rgb(0, 0, 0), Rgb(80, 0, 10), Rgb(170, 0, 40), Rgb(230, 40, 0), Rgb(255, 110, 0),
                              Rgb(140, 0, 60)};
  static const Rgb kRainbow[] = {Rgb(255, 0, 0), Rgb(255, 255, 0), Rgb(0, 255, 0), Rgb(0, 255, 255),
                                 Rgb(0, 0, 255), Rgb(255, 0, 255)};
  static const Rgb kParty[] = {Rgb(90, 0, 255), Rgb(255, 0, 120), Rgb(255, 60, 0), Rgb(255, 200, 0),
                               Rgb(0, 200, 255), Rgb(0, 60, 255)};
  switch (p) {
    case Palette::Ocean: return {kOcean, 6, true};
    case Palette::Forest: return {kForest, 6, true};
    case Palette::Lava: return {kLava, 6, false};
    case Palette::Rainbow: return {kRainbow, 6, true};
    case Palette::Party: return {kParty, 6, true};
    case Palette::Heat:
    default: return {kHeat, 7, false};
  }
}

// Color at `index` (0..255). A wrapping palette treats the stops as a ring
// (index 255 is nearly back at the first color), so effects that scroll
// through it never jump; a non-wrapping one runs from the first stop at 0 to
// the last at 255.
inline Rgb paletteColor(Palette p, uint8_t index) {
  const PaletteStops s = paletteStops(p);
  const uint32_t segments = s.wraps ? s.count : s.count - 1u;
  const uint32_t pos = static_cast<uint32_t>(index) * segments;  // 0 .. segments * 255
  uint32_t i = pos / 255;                                          // which pair of stops
  uint8_t t = static_cast<uint8_t>(pos % 255 * 255 / 254);         // 0..254 -> 0..255 between them
  if (i >= segments) {  // index 255: exactly the end of the last segment
    i = segments - 1;
    t = 255;
  }
  return lerp(s.stops[i], s.stops[(i + 1) % s.count], t);
}

// ---- Parameters ----

struct EffectParams {
  uint8_t speed = 128;       // 1..255; 128 is each effect's natural pace, 255 twice as fast
  uint8_t intensity = 128;   // 1..255; fire: how high the flames reach, sparks: how many, relax: contrast,
                             // music effects: sensitivity (how quiet a sound still shows)
  Palette palette = Palette::Auto;
  uint8_t brightness = 255;  // 1..255; the effect's own, independent of the bins' brightness
  uint16_t timerMin = 0;     // stop after this many minutes; 0 = keep running
};

// The palette an effect uses when the parameter says "auto".
inline Palette effectivePalette(GridEffect e, Palette p) {
  if (p != Palette::Auto) return p;
  switch (e) {
    case GridEffect::Sparks:
    case GridEffect::Beat: return Palette::Party;
    case GridEffect::Relax: return Palette::Ocean;
    case GridEffect::Spectrum: return Palette::Rainbow;  // unused: Auto draws a level meter (see renderSpectrum)
    default: return Palette::Heat;
  }
}

// ---- Random numbers ----
//
// xorshift32: tiny, fast and good enough for flicker. Seeded by the caller
// (the hardware random generator on the board, a fixed number in the tests).
class Random {
 public:
  explicit Random(uint32_t seed = 0x9E3779B9u) { reseed(seed); }
  void reseed(uint32_t seed) { state_ = seed ? seed : 0x9E3779B9u; }
  uint32_t next() {
    uint32_t x = state_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return state_ = x;
  }
  // 0 .. n-1 (n > 0).
  uint32_t below(uint32_t n) { return static_cast<uint32_t>((static_cast<uint64_t>(next()) * n) >> 32); }
  // lo .. hi inclusive.
  uint8_t range8(uint8_t lo, uint8_t hi) { return static_cast<uint8_t>(lo + below(hi - lo + 1u)); }

 private:
  uint32_t state_;
};

// ---- One running effect ----

class EffectState {
 public:
  // Simulation steps run on a fixed tick of effect time (scaled by speed), so
  // the pictures look the same at any frame rate and after a late frame.
  static constexpr uint32_t kFireStepMs = 40;
  static constexpr uint32_t kSparkStepMs = 20;
  // A frame that comes very late (a long flash write) runs at most this many
  // steps instead of catching up on all of them at once.
  static constexpr uint32_t kMaxStepsPerFrame = 6;

  void begin(GridEffect effect, uint8_t cols, uint8_t rows, uint32_t nowMs, uint32_t seed) {
    effect_ = effect;
    cols_ = cols;
    rows_ = rows;
    const size_t cells = static_cast<size_t>(cols) * rows;
    random_.reseed(seed);
    lastMs_ = nowMs;
    scaledMs_ = 0;
    pendingMs_ = 0;
    // Only the effect that needs a buffer gets one; the others free theirs.
    heat_.assign(effect == GridEffect::Fire ? cells : 0, 0);
    prevHeat_.assign(heat_.size(), 0);
    const bool flashes = effect == GridEffect::Sparks || effect == GridEffect::Beat;
    sparkLevel_.assign(flashes ? cells : 0, 0);
    sparkColor_.assign(sparkLevel_.size(), 0);
    bars_.assign(effect == GridEffect::Spectrum ? cols : 0, 0.0f);
    pulse_ = 0;
    beatsSeen_ = false;
  }

  GridEffect effect() const { return effect_; }

  // Writes one color per bin into out[y * cols + x] (y = 0 is the top row).
  // `audio` is the latest analysis of the microphone (silence without one);
  // only the music effects look at it.
  void render(const EffectParams &p, uint32_t nowMs, const AudioFrame &audio, Rgb *out) {
    // Effect time runs at speed/128 of real time: 1 is very slow, 255 about
    // twice the natural pace. Kept as a running sum so speed changes never
    // make the picture jump.
    const uint32_t dt = nowMs - lastMs_;
    lastMs_ = nowMs;
    const uint32_t scaled = (dt > 1000 ? 1000 : dt) * (p.speed ? p.speed : 1) / 128;
    scaledMs_ += scaled;
    pendingMs_ += scaled;
    frameMs_ = scaled;
    const Palette pal = effectivePalette(effect_, p.palette);
    switch (effect_) {
      case GridEffect::Fire: renderFire(p, pal, out); break;
      case GridEffect::Sparks: renderSparks(p, pal, out); break;
      case GridEffect::Relax: renderRelax(p, pal, out); break;
      case GridEffect::Spectrum: renderSpectrum(p, pal, audio, out); break;
      case GridEffect::Beat: renderBeat(p, pal, audio, out); break;
      case GridEffect::Pulse: renderPulse(p, pal, audio, out); break;
      default: std::fill(out, out + cells(), Rgb()); break;
    }
  }

 private:
  size_t cells() const { return static_cast<size_t>(cols_) * rows_; }
  size_t at(uint8_t x, uint8_t y) const { return static_cast<size_t>(y) * cols_ + x; }

  // Takes the simulation steps that are due; returns how far (0..255) the
  // clock is into the next one, for blending between the last two states.
  template <typename Step>
  uint8_t advance(uint32_t stepMs, Step step) {
    uint32_t steps = 0;
    while (pendingMs_ >= stepMs) {
      pendingMs_ -= stepMs;
      if (steps < kMaxStepsPerFrame) step();
      steps++;
    }
    return static_cast<uint8_t>(pendingMs_ * 255 / stepMs);
  }

  // Fire: a column of heat per grid column, the classic "Fire2012" model
  // turned on its side to the grid. Each step every cell cools a little,
  // heat drifts upwards and spreads sideways, and random sparks of new heat
  // start in the bottom row. Heat is shown through the palette (black, red,
  // orange, yellow, white for heat). Frames between steps blend the last two
  // states, so the flames move smoothly at 60 fps although they only change
  // 25 times a second.
  //
  // Intensity sets how high the flames reach: low means fast cooling (a low
  // glow along the bottom), high means slow cooling and more sparks.
  void renderFire(const EffectParams &p, Palette pal, Rgb *out) {
    const uint8_t blend = advance(kFireStepMs, [&] { fireStep(p.intensity); });
    for (size_t i = 0; i < cells(); i++) {
      const uint8_t h = lerp8(prevHeat_[i], heat_[i], blend);
      // Heat 0..255 maps onto the lower 240 of the palette: the very top is
      // white, which looks better as a rare highlight than as the flame body.
      out[i] = paletteColor(pal, static_cast<uint8_t>(h * 240 / 255));
    }
  }

  void fireStep(uint8_t intensity) {
    prevHeat_ = heat_;
    const uint8_t rows = rows_;
    // Cooling per step, scaled to the number of rows: with fewer rows the
    // heat has less room to fade, so each row has to cool more.
    const uint32_t cooling = 20 + (255 - intensity) * 70 / 255;  // 20 (tall) .. 90 (low)
    const uint32_t maxCool = cooling * 10 / rows + 2;
    for (uint8_t x = 0; x < cols_; x++) {
      for (uint8_t y = 0; y < rows; y++) {
        uint8_t &h = heat_[at(x, y)];
        const uint32_t c = random_.below(maxCool + 1);
        h = h > c ? static_cast<uint8_t>(h - c) : 0;
      }
      // Heat rises: each cell takes from the two below it (y grows downwards),
      // mostly from the nearer one. Top first, so every cell reads values
      // from before this step. (The original weights the farther cell double,
      // which on a strip of 60 is fine but on 6 rows leaves every other row
      // dark.) The row above the bottom one mixes its own heat with the
      // bottom row's.
      for (uint8_t y = 0; y + 2 < rows; y++) {
        heat_[at(x, y)] = static_cast<uint8_t>((2u * heat_[at(x, y + 1)] + heat_[at(x, y + 2)]) / 3u);
      }
      if (rows >= 2) {
        const uint8_t y = static_cast<uint8_t>(rows - 2);
        heat_[at(x, y)] = static_cast<uint8_t>((heat_[at(x, y)] + 2u * heat_[at(x, y + 1)]) / 3u);
      }
      // New sparks in the bottom row.
      const uint32_t sparking = 50 + intensity * 150 / 255;  // chance out of 255
      if (random_.below(255) < sparking) {
        uint8_t &h = heat_[at(x, static_cast<uint8_t>(rows - 1))];
        const uint32_t add = random_.range8(160, 255);
        h = static_cast<uint8_t>(h + add > 255 ? 255 : h + add);
      }
    }
    // Sideways spread: a little of each neighbour's heat, so the flames hang
    // together as a fire rather than flickering as separate columns.
    if (cols_ >= 3) {
      for (uint8_t y = 0; y < rows; y++) {
        uint8_t left = heat_[at(0, y)];
        for (uint8_t x = 1; x + 1 < cols_; x++) {
          const uint8_t self = heat_[at(x, y)];
          heat_[at(x, y)] = static_cast<uint8_t>((left + 2u * self + heat_[at(x + 1, y)]) / 4u);
          left = self;
        }
      }
    }
  }

  // Sparks: bins flash up in random palette colors and fade away. Intensity
  // sets how many appear; speed how quickly (through effect time).
  void renderSparks(const EffectParams &p, Palette pal, Rgb *out) {
    advance(kSparkStepMs, [&] { sparkStep(p.intensity); });
    for (size_t i = 0; i < cells(); i++) out[i] = scale(paletteColor(pal, sparkColor_[i]), sparkLevel_[i]);
  }

  void sparkStep(uint8_t intensity) {
    // Fade: about 8 % per step, roughly a second from full to dark.
    for (uint8_t &l : sparkLevel_) l = static_cast<uint8_t>(l * 235u / 256u);
    // New sparks: on average cells x intensity / 6000 per step, i.e. with 60
    // bins and intensity 128, one new spark every second step.
    const uint32_t chance = static_cast<uint32_t>(cells()) * intensity;  // out of 6000
    uint32_t budget = chance;
    while (budget > 0) {
      const uint32_t roll = random_.below(6000);
      if (roll < (budget > 6000 ? 6000 : budget)) {
        const size_t i = random_.below(static_cast<uint32_t>(cells()));
        if (sparkLevel_[i] < 128) {  // a fresh spark, not one on a still-bright bin
          sparkLevel_[i] = 255;
          sparkColor_[i] = static_cast<uint8_t>(random_.next());
        }
      }
      budget = budget > 6000 ? budget - 6000 : 0;
    }
  }

  // Relax: soft color fields drifting slowly across the grid and breathing
  // in brightness, like a lava lamp. A pure function of effect time: three
  // slow waves at different angles and periods added together pick the
  // palette color, a fourth sets the brightness. Intensity is the contrast
  // of the breathing (low: nearly even light, high: deep, slow pulses).
  void renderRelax(const EffectParams &p, Palette pal, Rgb *out) {
    const float t = scaledMs_ / 1000.0f;  // effect seconds
    const uint8_t minLevel = static_cast<uint8_t>(255 - p.intensity * 200 / 255);  // 255 .. 55
    for (uint8_t y = 0; y < rows_; y++) {
      for (uint8_t x = 0; x < cols_; x++) {
        const float fx = x * 0.55f, fy = y * 0.55f;
        const float v = std::sin(fx * 0.9f + t * 0.21f) + std::sin(fy * 1.1f - t * 0.17f) +
                        std::sin((fx + fy) * 0.6f + t * 0.13f);  // -3 .. 3
        const uint8_t index = static_cast<uint8_t>((v + 3.0f) * (255.0f / 6.0f));
        const float b = 0.5f + 0.5f * std::sin(fx * 0.4f - fy * 0.5f + t * 0.31f);  // 0 .. 1
        const uint8_t level = static_cast<uint8_t>(minLevel + b * (255 - minLevel));
        out[at(x, y)] = scale(paletteColor(pal, index), level);
      }
    }
  }

  // ---- Music effects ----
  //
  // Sound levels are shown over a window kMusicRangeDb wide that ends at the
  // analyzer's auto-gain reference (the recent loudest). The window never
  // starts below the gate, which comes from the intensity slider: at 128
  // anything quieter than about -60 dB (a quiet room) stays dark, at 255 the
  // limit is -80 dB (very sensitive), at 1 it is -40 dB (only loud music).
  static constexpr float kMusicRangeDb = 30.0f;

  static float gateDb(uint8_t intensity) { return -40.0f - intensity * 40.0f / 255.0f; }

  // 0..1: how far `db` reaches into the window.
  static float musicLevel(float db, float refDb, uint8_t intensity) {
    const float lowest = gateDb(intensity) + kMusicRangeDb;
    const float top = refDb > lowest ? refDb : lowest;
    const float v = (db - (top - kMusicRangeDb)) / kMusicRangeDb;
    return v < 0 ? 0 : v > 1 ? 1 : v;
  }

  // A level meter's colors: green (0) through yellow (about 60 %) to red
  // (255).
  static Rgb meterColor(uint8_t h) {
    const uint8_t kYellow = 153;
    if (h <= kYellow) return Rgb(static_cast<uint8_t>(h * 255 / kYellow), 255, 0);
    return Rgb(255, static_cast<uint8_t>(255 - (h - kYellow) * 255 / (255 - kYellow)), 0);
  }

  // Spectrum: one bar per column, bass on the left, treble on the right,
  // growing from the bottom row; the top cell of a bar lights partly for the
  // part of a row it reaches. Bars jump up at once and sink at a steady rate
  // (speed sets how fast), so they look lively without flickering. Colors
  // go by height, like a level meter: with the effect's own colors green at
  // the bottom through yellow to red at the top; with a chosen palette from
  // its low end at the bottom to its high end at the top.
  void renderSpectrum(const EffectParams &p, Palette pal, const AudioFrame &a, Rgb *out) {
    const int bands = AudioFrame::kBands;
    const float fall = frameMs_ / 700.0f;  // full height to nothing in 0.7 s at speed 128
    for (uint8_t x = 0; x < cols_; x++) {
      // The bands under this column: with fewer columns than bands each
      // column shows the loudest of its bands, with more, bands repeat.
      int b0 = x * bands / cols_, b1 = (x + 1) * bands / cols_;
      if (b1 <= b0) b1 = b0 + 1;
      float v = 0;
      for (int b = b0; b < b1; b++) {
        const float l = musicLevel(a.bandDb[b], a.peakDb, p.intensity);
        if (l > v) v = l;
      }
      float &bar = bars_[x];
      bar = v > bar ? v : (bar - fall > v ? bar - fall : v);
      const float height = bar * rows_;  // in rows
      for (uint8_t k = 0; k < rows_; k++) {  // k = 0 is the bottom row (y = rows - 1)
        float fill = height - k;
        fill = fill < 0 ? 0 : fill > 1 ? 1 : fill;
        const uint8_t h = static_cast<uint8_t>(rows_ > 1 ? k * 255 / (rows_ - 1) : 0);  // 0 bottom .. 255 top
        const Rgb color = p.palette == Palette::Auto ? meterColor(h) : paletteColor(pal, h);
        out[at(x, static_cast<uint8_t>(rows_ - 1 - k))] = scale(color, static_cast<uint8_t>(fill * 255));
      }
    }
  }

  // Beat: every beat lights a round splash of one color at a random spot,
  // bigger for a stronger beat (up to most of the grid), which then fades
  // (speed sets how fast). Splashes overlap like sparks do.
  void renderBeat(const EffectParams &p, Palette pal, const AudioFrame &a, Rgb *out) {
    advance(kSparkStepMs, [&] {
      for (uint8_t &l : sparkLevel_) l = static_cast<uint8_t>(l * 225u / 256u);
    });
    if (!beatsSeen_) {  // a beat counted before this effect started is not one of ours
      beats_ = a.beats;
      beatsSeen_ = true;
    }
    if (a.beats != beats_) {
      beats_ = a.beats;
      const float strength = musicLevel(a.bassDb, a.bassPeakDb, p.intensity);
      if (strength > 0) splash(strength);
    }
    for (size_t i = 0; i < cells(); i++) out[i] = scale(paletteColor(pal, sparkColor_[i]), sparkLevel_[i]);
  }

  void splash(float strength) {
    const float cx = static_cast<float>(random_.below(cols_)), cy = static_cast<float>(random_.below(rows_));
    const float maxR = (cols_ > rows_ ? cols_ : rows_) / 2.0f;
    const float r = 0.8f + strength * maxR;
    const uint8_t color = static_cast<uint8_t>(random_.next());
    for (uint8_t y = 0; y < rows_; y++) {
      for (uint8_t x = 0; x < cols_; x++) {
        const float d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy));
        if (d > r) continue;
        const uint8_t level = static_cast<uint8_t>(255.0f * (1.0f - 0.6f * d / r));
        uint8_t &l = sparkLevel_[at(x, y)];
        if (level >= l) {
          l = level;
          sparkColor_[at(x, y)] = color;
        }
      }
    }
  }

  // Pulse: a glow from the middle of the grid that swells with the loudness
  // of the music, filling the whole grid at the loudest. The palette runs
  // from the centre (its top end: white-yellow on heat) outwards (red).
  // Loudness jumps up at once and sinks at a pace set by speed.
  void renderPulse(const EffectParams &p, Palette pal, const AudioFrame &a, Rgb *out) {
    const float v = musicLevel(a.levelDb, a.levelPeakDb, p.intensity);
    const float fall = frameMs_ / 500.0f;
    pulse_ = v > pulse_ ? v : (pulse_ - fall > v ? pulse_ - fall : v);
    const float cx = (cols_ - 1) / 2.0f, cy = (rows_ - 1) / 2.0f;
    const float rx = cols_ / 2.0f, ry = rows_ / 2.0f;
    const float reach = pulse_ * 1.2f;  // a little past the corners at full loudness
    for (uint8_t y = 0; y < rows_; y++) {
      for (uint8_t x = 0; x < cols_; x++) {
        const float dx = (x - cx) / rx, dy = (y - cy) / ry;
        const float d = std::sqrt((dx * dx + dy * dy) / 2.0f);  // 0 centre .. ~1 corners
        float lit = (reach - d) * 4.0f;                         // soft edge a quarter wide
        lit = lit < 0 ? 0 : lit > 1 ? 1 : lit;
        const float dc = d > 1 ? 1 : d;
        out[at(x, y)] = scale(paletteColor(pal, static_cast<uint8_t>(250 - dc * 190)), static_cast<uint8_t>(lit * 255));
      }
    }
  }

  GridEffect effect_ = GridEffect::Off;
  uint8_t cols_ = 0, rows_ = 0;
  Random random_;
  uint32_t lastMs_ = 0;
  uint32_t scaledMs_ = 0;   // effect time since begin(), in ms
  uint32_t pendingMs_ = 0;  // effect time not yet consumed by simulation steps
  std::vector<uint8_t> heat_, prevHeat_;
  std::vector<uint8_t> sparkLevel_, sparkColor_;  // sparks, and beat's splashes
  uint32_t frameMs_ = 0;      // effect time this frame
  std::vector<float> bars_;   // spectrum: shown height per column, 0..1
  float pulse_ = 0;           // pulse: shown loudness, 0..1
  uint32_t beats_ = 0;        // beat: the analyzer's beat count last seen
  bool beatsSeen_ = false;
};

// ---- The player ----
//
// Holds the effect that is showing and, during a change, the one it replaces:
// both render, and the old one is blended into the new one over kFadeMs (from
// and to dark when starting or stopping). Demo shows fire, sparks and relax
// in turn for kDemoMs each, with the same cross-fade.
class EffectPlayer {
 public:
  static constexpr uint32_t kFadeMs = 1500;
  static constexpr uint32_t kDemoMs = 30000;

  void configure(uint8_t cols, uint8_t rows) {
    cols_ = cols;
    rows_ = rows;
    const size_t cells = static_cast<size_t>(cols) * rows;
    a_.assign(cells, Rgb());
    b_.assign(cells, Rgb());
    c_.assign(cells, Rgb());
  }

  // How long a settings change takes to show in full (the grid's fade time,
  // config fadeMs). 0 applies changes at once.
  void setParamFadeMs(uint32_t ms) { paramFadeMs_ = ms; }

  // Changes the running effect's settings. They don't jump: speed, intensity
  // and brightness move from the values showing right now to the new ones
  // over the fade time, and a new palette cross-fades from the old colors, so
  // a slider let go or a palette picked looks like every other change on the
  // grid. A change that arrives in the middle of such a fade starts from where
  // that fade had got to.
  void setParams(const EffectParams &p, uint32_t nowMs) {
    // A new timer counts from now; an unchanged one keeps running.
    if (p.timerMin != params_.timerMin) setTimer(p.timerMin, nowMs);
    const uint8_t t = paramProgress(nowMs);
    from_ = shownParams(t);
    // The palette can't be half-way between two: the one that dominates the
    // picture at this moment is the starting point.
    fromPalette_ = t < 128 ? fromPalette_ : params_.palette;
    params_ = p;
    paramsAt_ = nowMs;
  }
  // The settings as set (where a fade in progress is heading).
  const EffectParams &params() const { return params_; }

  // The latest sound analysis, for the music effects.
  void setAudio(const AudioFrame &a) { audio_ = a; }

  // Starts `effect` (Off fades the current one out) with the current
  // settings. The timer starts over.
  void start(GridEffect effect, uint32_t nowMs, uint32_t seed) { start(effect, params_, nowMs, seed); }

  // The same with the effect's own settings (each effect remembers its own).
  // The effect fading out keeps the settings it ran with until it is gone,
  // so it doesn't jump to the new effect's speed, colors or brightness.
  void start(GridEffect effect, const EffectParams &p, uint32_t nowMs, uint32_t seed) {
    // The outgoing effect fades out with the settings it shows right now
    // (a settings fade may be in progress), in the palette that dominates.
    const uint8_t t = paramProgress(nowMs);
    EffectParams old = shownParams(t);
    old.palette = t < 128 ? fromPalette_ : params_.palette;
    if (effect != GridEffect::Off) params_ = p;
    // The new effect starts with its own settings as they are: the cross-fade
    // between the two effects already covers the change.
    from_ = params_;
    fromPalette_ = params_.palette;
    paramsAt_ = nowMs - paramFadeMs_;
    selected_ = effect;
    demoIndex_ = 0;
    demoSince_ = nowMs;
    setTimer(params_.timerMin, nowMs);
    if (switchTo(effect == GridEffect::Demo ? kDemoOrder[0] : effect, nowMs, seed)) outgoing_ = old;
  }

  // What the user chose (Demo stays Demo while it cycles).
  GridEffect selected() const { return selected_; }

  // Seconds until the timer stops the effect; 0 without a timer or when off.
  uint32_t remainingS(uint32_t nowMs) const {
    if (selected_ == GridEffect::Off || !stopAt_) return 0;
    const int32_t left = static_cast<int32_t>(stopAt_ - nowMs);
    return left > 0 ? static_cast<uint32_t>(left + 999) / 1000 : 0;
  }

  // True while anything shows: an effect, or one fading out.
  bool active() const { return current_.effect() != GridEffect::Off || fadingOut_; }

  // Renders into out[y * cols + x]. `seed` is used when the demo moves on or
  // the timer runs out (a fresh random seed from the caller each frame is fine).
  bool render(uint32_t nowMs, uint32_t seed, Rgb *out) {
    const size_t cells = a_.size();
    if (selected_ != GridEffect::Off && stopAt_ && static_cast<int32_t>(nowMs - stopAt_) >= 0) {
      selected_ = GridEffect::Off;
      stopAt_ = 0;
      switchTo(GridEffect::Off, nowMs, seed);
    }
    if (selected_ == GridEffect::Demo && nowMs - demoSince_ >= kDemoMs) {
      demoSince_ = nowMs;
      demoIndex_ = static_cast<uint8_t>((demoIndex_ + 1) % kDemoCount);
      switchTo(kDemoOrder[demoIndex_], nowMs, seed);
    }
    if (!active()) {
      std::fill(out, out + cells, Rgb());
      return false;
    }
    // The settings of this frame, part way through a settings fade.
    const uint8_t pt = paramProgress(nowMs);
    const EffectParams shown = shownParams(pt);
    current_.render(shown, nowMs, audio_, a_.data());
    if (pt < 255 && fromPalette_ != params_.palette) {
      // A new palette: the same picture in the old colors, blended into the
      // new ones. Rendering the effect a second time for the same moment
      // changes nothing in it: no effect time has passed, so no simulation
      // step runs and nothing new appears (no spark, no beat); only the
      // colors are looked up again.
      EffectParams old = shown;
      old.palette = fromPalette_;
      current_.render(old, nowMs, audio_, c_.data());
      for (size_t i = 0; i < cells; i++) a_[i] = lerp(c_[i], a_[i], pt);
    }
    for (size_t i = 0; i < cells; i++) a_[i] = scale(a_[i], shown.brightness);
    const uint32_t since = nowMs - changedAt_;
    const bool fading = since < kFadeMs;
    if (fading) {
      const uint8_t t = rampUp(since, kFadeMs);
      if (previous_.effect() != GridEffect::Off) {
        previous_.render(outgoing_, nowMs, audio_, b_.data());
        for (size_t i = 0; i < cells; i++) b_[i] = scale(b_[i], outgoing_.brightness);
      } else {
        std::fill(b_.begin(), b_.end(), Rgb());
      }
      for (size_t i = 0; i < cells; i++) a_[i] = lerp(b_[i], a_[i], t);
    } else {
      fadingOut_ = false;
      if (previous_.effect() != GridEffect::Off) previous_.begin(GridEffect::Off, cols_, rows_, nowMs, 0);
    }
    std::copy(a_.begin(), a_.end(), out);
    return true;
  }

 private:
  static constexpr GridEffect kDemoOrder[] = {GridEffect::Fire, GridEffect::Sparks, GridEffect::Relax};
  static constexpr uint8_t kDemoCount = 3;

  // 0..255: how far the settings fade has got (255 = the new settings in full).
  uint8_t paramProgress(uint32_t nowMs) const {
    // Signed: a time from just before the change (another task's clock
    // reading) counts as "not started", not as long finished.
    const int32_t since = static_cast<int32_t>(nowMs - paramsAt_);
    if (since < 0) return 0;
    return rampUp(static_cast<uint32_t>(since), paramFadeMs_);
  }

  // The settings showing at fade progress `t`: speed, intensity and
  // brightness between the old and the new values; the palette and the timer
  // as set (the palette is cross-faded in render()).
  EffectParams shownParams(uint8_t t) const {
    EffectParams p = params_;
    p.speed = lerp8(from_.speed, params_.speed, t);
    p.intensity = lerp8(from_.intensity, params_.intensity, t);
    p.brightness = lerp8(from_.brightness, params_.brightness, t);
    return p;
  }

  void setTimer(uint16_t minutes, uint32_t nowMs) {
    stopAt_ = minutes ? nowMs + minutes * 60000u : 0;
    if (stopAt_ == 0 && minutes) stopAt_ = 1;  // 0 means "no timer"
  }

  // False when `effect` is already showing (nothing changes).
  bool switchTo(GridEffect effect, uint32_t nowMs, uint32_t seed) {
    if (effect == current_.effect()) return false;
    // The outgoing effect keeps running in previous_ during the fade. When a
    // change comes in the middle of a fade, the half-faded picture is not
    // kept: the newer of the two effects becomes the one fading out.
    std::swap(previous_, current_);
    outgoing_ = params_;  // start() replaces this with the settings from before
    current_.begin(effect, cols_, rows_, nowMs, seed);
    changedAt_ = nowMs;
    fadingOut_ = effect == GridEffect::Off && previous_.effect() != GridEffect::Off;
    return true;
  }

  uint8_t cols_ = 0, rows_ = 0;
  EffectParams params_;
  EffectParams outgoing_;  // the settings of the effect fading out
  // Settings fade: from_ is where speed, intensity and brightness started,
  // fromPalette_ the palette being faded out, paramsAt_ when the change came.
  EffectParams from_;
  Palette fromPalette_ = Palette::Auto;
  uint32_t paramsAt_ = 0;
  uint32_t paramFadeMs_ = 400;
  GridEffect selected_ = GridEffect::Off;
  EffectState current_, previous_;
  uint32_t changedAt_ = 0;
  bool fadingOut_ = false;
  uint32_t stopAt_ = 0;  // effect time at which the timer stops it; 0 = no timer
  uint8_t demoIndex_ = 0;
  uint32_t demoSince_ = 0;
  std::vector<Rgb> a_, b_;  // the current and the outgoing effect's frame
  std::vector<Rgb> c_;      // the current effect in the old palette, during a palette fade
  AudioFrame audio_;
};

}  // namespace led
}  // namespace sb
