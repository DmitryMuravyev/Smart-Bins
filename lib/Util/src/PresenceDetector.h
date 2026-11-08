#pragma once

// Turns a motion sensor's digital output into "someone arrived" and "everyone
// left" (Task 16), the moments the work lights switch on and off.
//
// A sensor's output is high while it senses someone. A person walking past,
// or stopping at the bench for a moment, gives a short high pulse (a PIR
// module holds its output for 2-3 s after each movement); someone who sits
// down to work keeps it high, or high with short gaps. So:
//
//   - arrived: the output has stayed high for `onDelayMs` without a break.
//     The delay should be longer than a PIR's hold time, or a single pass
//     would count;
//   - left: the output has stayed low for `offDelayMs` without a break.
//     A radar module (LD2410C) stays high while someone sits still; a PIR
//     goes low between movements, which the long off delay rides over.
//
// Only these two moments cause an action. Switching the lights by hand in
// between is left alone until the next arrival or departure: lights turned
// off while someone is there stay off, and lights turned on with nobody
// there stay on until someone comes and goes.
//
// Pure logic with the time passed in, so the host tests can drive it.

#include <cstdint>

namespace sb {

class PresenceDetector {
 public:
  enum class Event : uint8_t { None, Arrived, Left };

  void configure(uint32_t onDelayMs, uint32_t offDelayMs) {
    onDelayMs_ = onDelayMs;
    offDelayMs_ = offDelayMs;
  }

  // Feeds the output's level at `nowMs` (any rate; the main loop calls it
  // every few milliseconds). Returns the event this sample completes.
  Event update(bool high, uint32_t nowMs) {
    if (!started_) {
      // The first sample starts both clocks.
      started_ = true;
      since_ = nowMs;
      high_ = high;
    }
    if (high != high_) {
      high_ = high;
      since_ = nowMs;  // the level changed: its time starts over
    }
    const uint32_t held = nowMs - since_;
    if (!present_ && high_ && held >= onDelayMs_) {
      present_ = true;
      return Event::Arrived;
    }
    if (present_ && !high_ && held >= offDelayMs_) {
      present_ = false;
      return Event::Left;
    }
    return Event::None;
  }

  // Someone is at the bench (arrived and not yet left).
  bool present() const { return present_; }
  // The sensor's output at the last sample.
  bool signal() const { return high_; }

 private:
  uint32_t onDelayMs_ = 5000;
  uint32_t offDelayMs_ = 5 * 60000;
  bool started_ = false;
  bool high_ = false;
  bool present_ = false;
  uint32_t since_ = 0;
};

}  // namespace sb
