#pragma once

// Brightness envelopes: smooth fades and the locate highlight's pulse modes.
// All levels are 0..255 and all times are in milliseconds.

#include <cmath>
#include <cstdint>

namespace sb {
namespace led {

enum class PulseMode : uint8_t { Steady, Slow, Fast };

constexpr uint32_t kSlowPulsePeriodMs = 2400;
constexpr uint32_t kFastPulsePeriodMs = 700;
constexpr uint8_t kPulseMinLevel = 64;  // pulses breathe between 25 % and 100 %

// Linear ramp from 0 to 255 over `durationMs` (instant when the duration is 0).
inline uint8_t rampUp(uint32_t elapsedMs, uint32_t durationMs) {
  if (durationMs == 0 || elapsedMs >= durationMs) return 255;
  return static_cast<uint8_t>(elapsedMs * 255 / durationMs);
}

inline uint8_t rampDown(uint32_t elapsedMs, uint32_t durationMs) { return 255 - rampUp(elapsedMs, durationMs); }

// Body of the highlight after it has faded in. Pulses start at full brightness and
// follow a cosine, so they never jump.
inline uint8_t pulseLevel(PulseMode mode, uint32_t elapsedMs) {
  if (mode == PulseMode::Steady) return 255;
  const uint32_t period = mode == PulseMode::Slow ? kSlowPulsePeriodMs : kFastPulsePeriodMs;
  const float phase = static_cast<float>(elapsedMs % period) / period;
  const float wave = 0.5f + 0.5f * std::cos(phase * 6.2831853f);  // 1 -> 0 -> 1
  return static_cast<uint8_t>(kPulseMinLevel + std::lround(wave * (255 - kPulseMinLevel)));
}

// A value that moves to a target over a fixed time (used for global brightness).
class Fader {
 public:
  explicit Fader(uint8_t value = 0) : from_(value), to_(value) {}

  void set(uint8_t target, uint32_t nowMs, uint32_t durationMs) {
    from_ = value(nowMs);
    to_ = target;
    start_ = nowMs;
    duration_ = durationMs;
  }

  uint8_t value(uint32_t nowMs) const {
    const uint32_t elapsed = nowMs - start_;
    if (duration_ == 0 || elapsed >= duration_) return to_;
    return static_cast<uint8_t>(from_ + (static_cast<int32_t>(to_) - from_) * static_cast<int32_t>(elapsed) /
                                            static_cast<int32_t>(duration_));
  }

  bool settled(uint32_t nowMs) const { return value(nowMs) == to_; }
  uint8_t target() const { return to_; }

 private:
  uint8_t from_;
  uint8_t to_;
  uint32_t start_ = 0;
  uint32_t duration_ = 0;
};

}  // namespace led
}  // namespace sb
