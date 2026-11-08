#include "MotionSensor.h"

// Arduino.h first: it routes ESP_LOGx through Arduino's logger (see Microphone.cpp).
#include <Arduino.h>
#include <esp_log.h>

#include "led/WorkLights.h"

namespace sb {

namespace {
const char *const TAG = "motion";
}  // namespace

void MotionSensor::begin(const config::AppConfig &cfg, led::WorkLights &lights) {
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
  detector_.configure(cfg.motion.onDelayS * 1000UL, cfg.motion.offDelayMin * 60000UL);
  ESP_LOGI(TAG, "Motion sensor on GPIO%d: lights on after %us of presence, off after %u min without",
           pin, cfg.motion.onDelayS, cfg.motion.offDelayMin);
}

void MotionSensor::loop() {
  if (pin_ < 0) return;
  const bool wasSignal = detector_.signal(), wasPresent = detector_.present();
  const PresenceDetector::Event e = detector_.update(digitalRead(pin_) == HIGH, millis());
  if (detector_.signal() != wasSignal || detector_.present() != wasPresent) version_ = version_ + 1;
  if (e == PresenceDetector::Event::Arrived) {
    // Every light on, at its own last level, color and effect (like the
    // switch on the Lights page). Lights switched off by hand stay off
    // until the next arrival (see PresenceDetector.h).
    ESP_LOGI(TAG, "Someone at the bench: work lights on");
    led::WorkLights::Change on;
    on.power = led::WorkLights::Change::Power::On;
    for (uint8_t i = 0; i < lights_->count(); i++) lights_->set(i, on);
  } else if (e == PresenceDetector::Event::Left) {
    ESP_LOGI(TAG, "Nobody at the bench: work lights off");
    lights_->allOff();
  }
}

}  // namespace sb
