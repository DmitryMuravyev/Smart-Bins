#pragma once

// Motion sensor: a master switch over the lights. When nobody has
// been at the bench for a while, every work light and the grid's effect fade
// to dark; when someone stays, they come back as they were. The lights'
// own settings (which are on, their levels, colors and effects) are never
// changed by it: the Lights page keeps showing what the user set, and a
// light switched off by hand stays off when someone arrives. Located bins
// and test patterns are not affected: someone at a page asked for them.
//
// With the LED supply under PS-ON control (PowerSupply.h), dark lights also
// mean the supply's main output goes off: nobody there, no power drawn.
//
// The board starts as if someone had just arrived (the off delay runs from
// boot). Switching a light or an effect on from a page, or the pages'
// "Turn back on" button, brings the lights back and keeps them on until
// the sensor has seen someone arrive and leave (hold()): whoever switched
// them on may be anywhere, and they stay on for them.
//
// The sensor's digital output (high = someone there) is read on the analog
// microphone's pin, pins.mic_analog (GPIO35 by default, an input-only pin,
// which suits a signal coming in). Suitable modules:
//   - LD2410C (24 GHz radar, 5 V supply, 3.3 V output): also senses someone
//     sitting still, and its range can be limited in its phone app (HLKRadarTool)
//     so that people further away in the room don't count. The best fit.
//   - HC-SR501 (PIR, 5-20 V supply, 3.3 V output): set its jumper to repeat
//     trigger (H) and its time pot to the minimum; the sensitivity pot
//     shortens its range.
//   - AM312 (small PIR, 2.7-12 V, 2 s hold): fixed range of a few meters.
// The timing rules are in PresenceDetector.h; the delays are settings
// (config.motion), so a sensor's quirks can be ridden over.
//
// Polled from the main loop: a level read every few milliseconds is plenty
// for delays counted in seconds.

#include <cstdint>

#include "AppConfig.h"
#include "PresenceDetector.h"

namespace sb {

namespace led {
class LedEngine;
class WorkLights;
}

class MotionSensor {
 public:
  // Does nothing unless config.motion.enabled and the pin is set.
  void begin(const config::AppConfig &cfg, led::WorkLights &lights, led::LedEngine &grid);
  void loop();
  // Someone switched a light or an effect on from a page: the lights are
  // on (back) until the sensor has seen someone arrive and leave. Safe from
  // any task (picked up by the next loop()).
  void hold() { holdRequested_ = true; }

  bool enabled() const { return pin_ >= 0; }
  // Someone is at the bench, by the delays (what the lights follow).
  bool present() const { return detector_.present(); }
  // The sensor's output right now, for setting the sensor up.
  bool signal() const { return detector_.signal(); }
  // Increases whenever present() or signal() changes; the WebSocket pushes
  // the lights (which carry these two) when it moves.
  uint32_t version() const { return version_; }

 private:
  int pin_ = -1;
  led::WorkLights *lights_ = nullptr;
  led::LedEngine *grid_ = nullptr;
  volatile bool holdRequested_ = false;
  void setMaster(bool on);
  PresenceDetector detector_;
  volatile uint32_t version_ = 0;
};

}  // namespace sb
