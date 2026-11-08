#include "PowerSupply.h"

// Arduino.h first: it routes ESP_LOGx through Arduino's logger (see Microphone.cpp).
#include <Arduino.h>
#include <esp_log.h>

namespace sb {

namespace {
const char *const TAG = "supply";

// After the last light lets go (each one already waits 1.5s after going
// dark): rides over one light letting go just before another asks.
constexpr uint32_t kOffDelayMs = 1000;
// Without PG: time the main output gets to come up after PS-ON. The
// EPP-300-12 signals PG 10-500 ms after its output is up, so its output is
// up within that; data sent earlier would be lost.
constexpr uint32_t kSettleMs = 500;
// With PG: how long PG may take to come up before it counts as a fault.
constexpr uint32_t kPgTimeoutMs = 2000;
}  // namespace

void PowerSupply::begin(int psOnPin, int pgPin) {
  gate_.configure(kOffDelayMs, kSettleMs, kPgTimeoutMs);
  if (psOnPin < 0) return;
  // Low at once: the main output stays off until a light needs it (the
  // pull-down on the board holds it low while the ESP32 boots).
  pinMode(psOnPin, OUTPUT);
  digitalWrite(psOnPin, LOW);
  psOn_ = psOnPin;
  if (pgPin >= 0) {
    // GPIO34-39 have no internal pulls; the divider on the board sets the level.
    pinMode(pgPin, INPUT);
    pg_ = pgPin;
  }
  ESP_LOGI(TAG, "LED supply switched by PS-ON on GPIO%d%s", psOnPin,
           pgPin >= 0 ? "" : ", no power-good input (waits 500 ms after switching on)");
  if (pgPin >= 0) ESP_LOGI(TAG, "Power good (PG) on GPIO%d", pgPin);
}

bool PowerSupply::want(User user, bool needed) {
  if (psOn_ < 0) return true;
  // Read outside the lock: a GPIO read is quick, but nothing slow belongs
  // inside a spinlock.
  const bool pg = pg_ >= 0 && digitalRead(pg_) == HIGH;
  const uint32_t now = millis();
  SupplyGate::Step step;
  bool switched;
  portENTER_CRITICAL(&mux_);
  if (needed) wanted_ |= user;
  else wanted_ &= ~user;
  step = gate_.update(wanted_ != 0, pg_ >= 0, pg, now);
  switched = step.psOn != last_.psOn;
  if (switched) digitalWrite(psOn_, step.psOn ? HIGH : LOW);
  if (switched || step.ready != last_.ready) version_ = version_ + 1;
  last_ = step;
  portEXIT_CRITICAL(&mux_);
  if (switched) ESP_LOGI(TAG, "LED supply %s", step.psOn ? "on" : "off");
  if (step.fault == SupplyGate::Fault::PgLost) {
    ESP_LOGE(TAG, "Power good (PG) dropped while the supply was on: overload or supply fault?");
  } else if (step.fault == SupplyGate::Fault::PgLate) {
    ESP_LOGW(TAG, "Power good (PG) not high %u ms after switching on: lights go on anyway (check the PG wiring)",
             (unsigned)kPgTimeoutMs);
  }
  return step.ready;
}

PowerSupply::Status PowerSupply::status() {
  portENTER_CRITICAL(&mux_);
  const Status s{psOn_ >= 0, last_.psOn, psOn_ < 0 || last_.ready, pg_ >= 0};
  portEXIT_CRITICAL(&mux_);
  return s;
}

}  // namespace sb
