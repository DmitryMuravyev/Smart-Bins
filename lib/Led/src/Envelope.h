#pragma once

// Brightness envelopes: smooth fades and the locate highlight's pulse modes.
// All 8-bit levels are 0..255 and all times are in milliseconds.
//
// Everything here is a pure function of elapsed time rather than a value that
// is stepped once per frame. A frame that arrives late (Wi-Fi, a flash write)
// therefore lands exactly where it should on the curve instead of making the
// whole fade slower, and the same code runs unchanged in the host unit tests.
//
// Elapsed times are computed as `now - start` on uint32_t millis(). Unsigned
// subtraction stays correct across the 49.7-day millis() wrap-around, so no
// code here ever compares two absolute timestamps directly.

#include <cmath>
#include <cstdint>

namespace sb {
namespace led {

enum class PulseMode : uint8_t { Steady, Slow, Fast };

constexpr uint32_t kSlowPulsePeriodMs = 2400;
constexpr uint32_t kFastPulsePeriodMs = 700;
constexpr uint8_t kPulseMinLevel = 64;  // pulses breathe between 25 % and 100 %

// Linear ramp from 0 to 255 over `durationMs` (instant when the duration is 0).
// Linear is enough here because the result is gamma-corrected before output,
// which already makes the ramp look even to the eye.
inline uint8_t rampUp(uint32_t elapsedMs, uint32_t durationMs) {
  if (durationMs == 0 || elapsedMs >= durationMs) return 255;
  return static_cast<uint8_t>(elapsedMs * 255 / durationMs);
}

inline uint8_t rampDown(uint32_t elapsedMs, uint32_t durationMs) { return 255 - rampUp(elapsedMs, durationMs); }

// Raised cosine between `minLevel` and 255 with the given period. It starts at
// 255 (phase 0), so a pulse that begins right after a fade-in continues from
// full brightness without a jump, and the cosine has no corners, so the
// turnarounds at the top and bottom look soft rather than "bouncing".
inline uint8_t cosineWave(uint32_t elapsedMs, uint32_t periodMs, uint8_t minLevel) {
  const float phase = static_cast<float>(elapsedMs % periodMs) / periodMs;
  const float wave = 0.5f + 0.5f * std::cos(phase * 6.2831853f);  // 1 -> 0 -> 1
  return static_cast<uint8_t>(minLevel + std::lround(wave * (255 - minLevel)));
}

// Body of the highlight after it has faded in.
inline uint8_t pulseLevel(PulseMode mode, uint32_t elapsedMs) {
  if (mode == PulseMode::Steady) return 255;
  const uint32_t period = mode == PulseMode::Slow ? kSlowPulsePeriodMs : kFastPulsePeriodMs;
  return cosineWave(elapsedMs, period, kPulseMinLevel);
}

// A value that moves linearly to a target over a fixed time.
//
// set() snapshots the value *at that moment* as the new starting point, so a
// change that arrives in the middle of a fade (for example "off" while still
// fading on) reverses smoothly from wherever the output currently is instead
// of jumping to the old target first. The fade always takes the full duration,
// whatever the distance, which keeps every transition in the UI the same length.
//
// T is uint8_t for 8-bit outputs (addressable pixels) or uint16_t for the
// 12-bit PWM work lights, where 256 steps would show visible stairs at the dark
// end of a slow fade.
template <typename T>
class BasicFader {
 public:
  explicit BasicFader(T value = 0) : from_(value), to_(value) {}

  void set(T target, uint32_t nowMs, uint32_t durationMs) {
    from_ = value(nowMs);
    to_ = target;
    start_ = nowMs;
    duration_ = durationMs;
  }

  // Jumps straight to `target` (used on start-up before anything is visible).
  void reset(T target) {
    from_ = to_ = target;
    duration_ = 0;
  }

  T value(uint32_t nowMs) const {
    const uint32_t elapsed = nowMs - start_;
    if (duration_ == 0 || elapsed >= duration_) return to_;
    // 64-bit intermediate: for uint16_t, 65535 x 5000 ms already exceeds int32.
    const int64_t delta = static_cast<int64_t>(to_) - from_;
    return static_cast<T>(from_ + delta * static_cast<int64_t>(elapsed) / static_cast<int64_t>(duration_));
  }

  bool settled(uint32_t nowMs) const { return value(nowMs) == to_; }
  T target() const { return to_; }

 private:
  T from_;
  T to_;
  uint32_t start_ = 0;
  uint32_t duration_ = 0;
};

using Fader = BasicFader<uint8_t>;
using Fader16 = BasicFader<uint16_t>;

}  // namespace led
}  // namespace sb
