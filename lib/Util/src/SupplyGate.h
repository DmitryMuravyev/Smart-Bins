#pragma once

// When the LED supply's main output should be on (PS-ON), and when it is up.
//
// A supply with remote control (Mean Well EPP-300-12 and similar) keeps its
// 5 V standby output on for the ESP32 and switches its main 12 V output with
// the PS-ON input. The firmware turns the main output on as soon as any light
// needs it (the grid, the addressable work strips or a plain work light) and
// off once none has needed it for `offDelayMs`. Each of those already waits
// until its own LEDs have faded to dark (and 1.5s more) before it lets go, so
// the delay here only rides over one of them letting go just before another
// asks: the supply doesn't click off and on in between.
//
// "Up" (the LEDs may be sent data and their switches closed):
//   - with the PG (power good) output wired: when PG is high. A PG that
//     hasn't come up `pgTimeoutMs` after PS-ON counts as a fault (reported
//     once), and the output is then taken as up anyway, so a PG wiring
//     mistake doesn't leave every light dark;
//   - without PG: `settleMs` after PS-ON.
// PG going low while the output is on is a fault too (a failing supply or an
// overload), reported once per drop.
//
// Pure logic with the time and the PG level passed in, so the host tests can
// drive it; PowerSupply (src/PowerSupply.h) adds the pins.

#include <cstdint>

namespace sb {

class SupplyGate {
 public:
  enum class Fault : uint8_t { None, PgLate, PgLost };

  struct Step {
    bool psOn;    // drive PS-ON high
    bool ready;   // the main output is up
    Fault fault;  // a fault this step found (reported once)
  };

  void configure(uint32_t offDelayMs, uint32_t settleMs, uint32_t pgTimeoutMs) {
    offDelayMs_ = offDelayMs;
    settleMs_ = settleMs;
    pgTimeoutMs_ = pgTimeoutMs;
  }

  // `wanted`: some light needs the supply now. `pg`: the PG input's level,
  // ignored when `hasPg` is false.
  Step update(bool wanted, bool hasPg, bool pg, uint32_t nowMs) {
    Fault fault = Fault::None;
    if (wanted) {
      lastWanted_ = nowMs;
      if (!on_) {
        on_ = true;
        onAt_ = nowMs;
        up_ = false;
        pgLate_ = false;
      }
    } else if (on_ && nowMs - lastWanted_ >= offDelayMs_) {
      on_ = false;
      up_ = false;
    }
    if (on_) {
      const uint32_t since = nowMs - onAt_;
      if (!hasPg) {
        up_ = since >= settleMs_;
      } else if (pg) {
        up_ = true;
        pgLate_ = false;
      } else if (up_ && !pgLate_) {
        // It was up and PG dropped.
        up_ = false;
        fault = Fault::PgLost;
        lostAt_ = nowMs;
      } else if (!pgLate_ && since >= pgTimeoutMs_ && nowMs - lostAt_ >= pgTimeoutMs_) {
        // PG never came (back) up: carry on without it.
        pgLate_ = true;
        up_ = true;
        fault = Fault::PgLate;
      }
    }
    return {on_, on_ && up_, fault};
  }

 private:
  uint32_t offDelayMs_ = 1000;
  uint32_t settleMs_ = 300;
  uint32_t pgTimeoutMs_ = 2000;
  bool on_ = false;
  bool up_ = false;
  bool pgLate_ = false;  // going on without PG until it comes up
  uint32_t onAt_ = 0;
  uint32_t lastWanted_ = 0;
  uint32_t lostAt_ = 0;
};

}  // namespace sb
