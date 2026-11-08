#include "MotionSensor.h"

// Arduino.h first: it routes ESP_LOGx through Arduino's logger (see Microphone.cpp).
#include <Arduino.h>
#include <esp_log.h>

#include "led/LedEngine.h"
#include "led/WorkLights.h"

namespace sb {

namespace {
const char *const TAG = "motion";
}  // namespace

void MotionSensor::begin(const config::AppConfig &cfg, led::WorkLights &lights, led::LedEngine &grid) {
  if (!cfg.motion.enabled) return;
  const int pin = cfg.pins["mic_analog"];
  if (pin < 0) {
    ESP_LOGW(TAG, "On in the configuration, but the pin (mic_analog) is not set");
    return;
  }
  // No pull resistor: the modules drive their output both ways, and GPIO34-39
  // have no internal pulls anyway. An unconnected pin floats, which can look
  // like someone there now and then; the on delay keeps that from lighting up.
  pinMode(pin, INPUT);
  pin_ = pin;
  lights_ = &lights;
  grid_ = &grid;
  detector_.configure(cfg.motion.onDelayS * 1000UL, cfg.motion.offDelayMin * 60000UL);
  // As if someone had just arrived: the lights come up as saved, and go
  // dark after the off delay unless someone is sensed by then.
  detector_.wake(millis());
  ESP_LOGI(TAG, "Motion sensor on GPIO%d: lights back after %us of presence, dark after %u min without",
           pin, cfg.motion.onDelayS, cfg.motion.offDelayMin);
}

void MotionSensor::setMaster(bool on) {
  lights_->setMaster(on);
  grid_->setMaster(on);
}

void MotionSensor::loop() {
  if (pin_ < 0) return;
  const bool wasSignal = detector_.signal(), wasPresent = detector_.present();
  const uint32_t now = millis();
  if (holdRequested_) {
    holdRequested_ = false;
    detector_.hold();
    if (!wasPresent) {
      ESP_LOGI(TAG, "Switched on from a page: lights back until someone comes and goes");
      setMaster(true);
    }
  }
  const PresenceDetector::Event e = detector_.update(digitalRead(pin_) == HIGH, now);
  if (detector_.signal() != wasSignal || detector_.present() != wasPresent) version_ = version_ + 1;
  if (e == PresenceDetector::Event::Arrived) {
    ESP_LOGI(TAG, "Someone at the bench: lights back");
    setMaster(true);
  } else if (e == PresenceDetector::Event::Left) {
    ESP_LOGI(TAG, "Nobody at the bench: lights dark");
    setMaster(false);
  }
}

}  // namespace sb
