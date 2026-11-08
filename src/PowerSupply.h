#pragma once

// The LED supply's remote on/off (PS-ON) and power-good (PG) signals, for
// supplies that have them, such as the Mean Well EPP-300-12 (hardware guide,
// section 7). The ESP32 runs from the supply's 5 V standby output, and the
// main 12 V output is on only while some light needs it.
//
// Three users ask for it, each from its own output task and once per frame:
// the grid (LedEngine), the addressable work strips and the plain (PWM) work
// lights (both WorkLights). want() records the asker's need, switches PS-ON
// and answers whether the output is up, so a strip's own switch closes and
// data goes out only once there is power behind it. The timing rules are in
// SupplyGate.h.
//
// Without a PS-ON pin (pins.ps_on unset, the default) want() always answers
// "up" and nothing is switched: the supply is simply always on.

#include <cstdint>

#include <freertos/FreeRTOS.h>

#include "SupplyGate.h"

namespace sb {

class PowerSupply {
 public:
  enum User : uint8_t { Grid = 1, Strips = 2, Plain = 4 };

  struct Status {
    bool used;   // a PS-ON pin is set
    bool on;     // PS-ON is high
    bool ready;  // the main output is up
    bool pg;     // a PG pin is set
  };

  void begin(int psOnPin, int pgPin);
  // `user` needs the supply now (or not). Returns true when the main output
  // is up (always without a PS-ON pin).
  bool want(User user, bool needed);
  Status status();
  // Increases when on/ready change; the System page's figures follow it.
  uint32_t version() const { return version_; }

 private:
  int psOn_ = -1;
  int pg_ = -1;
  uint8_t wanted_ = 0;
  SupplyGate gate_;
  SupplyGate::Step last_{false, false, SupplyGate::Fault::None};
  volatile uint32_t version_ = 0;
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace sb
