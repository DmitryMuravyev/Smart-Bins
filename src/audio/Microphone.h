#pragma once

// The microphone for the music effects (Tasks 13 and 14).
//
// The microphone is optional, and the configuration says which kind is
// wired (config.json "microphone", the Hardware section of the Settings
// page):
//   i2s     an INMP441 (or similar) on pins.mic_i2s_sck/ws/sd, 16 kHz,
//           24-bit; whichever channel turns out to carry sound is used, so
//           its L/R pin may be tied to GND or 3.3 V;
//   analog  a MAX9814 (or similar) on pins.mic_analog, an ADC1 pin
//           (GPIO32-39), sampled at 20 kHz, the slowest the ESP32's ADC DMA
//           runs; noisier than I2S, so it is the fallback;
//   off     no microphone: the music effects stay hidden.
//
// begin() starts a small task that first checks whether the microphone
// answers; if not, the task ends, the LED engine is told there is no
// microphone, and the music effects stay hidden in the UI. If it does, the
// task frees the driver as well and waits: only while a music effect runs
// (and a few seconds after) does it read the microphone, every 256 samples
// through AudioAnalyzer, the result handed to the LED engine for the music
// effects. That keeps the driver's and the buffers' memory free the rest of
// the time.

#include <cstdint>

#include "AppConfig.h"

namespace sb {
namespace led {
class LedEngine;
}

namespace audio {

class Microphone {
 public:
  static constexpr uint32_t kI2sRate = 16000;
  // The ESP32's ADC DMA doesn't go below 20 kHz (SOC_ADC_SAMPLE_FREQ_THRES_LOW).
  static constexpr uint32_t kAnalogRate = 20000;

  // Starts the check (and, with a microphone, the reading) in the background,
  // with the microphone type and pins from the configuration. Nothing starts
  // when the type is off or a pin it needs is -1.
  void begin(const config::AppConfig &cfg, led::LedEngine &engine);

  // True once a microphone has been found (the check takes about half a
  // second after begin()).
  bool present() const { return present_; }

 private:
  static void taskEntry(void *arg);
  void run();

  // The driver for the configured type: open() starts it (nullptr on
  // failure; void * so this header needn't pull in the drivers), read()
  // waits for the next buffer of samples and converts them to mono floats,
  // full scale +-1, returning how many it wrote (at most `max`; 0 on a
  // timeout), close() frees it again.
  void *open();
  int read(void *handle, uint8_t *raw, float *out, int max);
  void close(void *handle);
  uint32_t sampleRate() const { return analog_ ? kAnalogRate : kI2sRate; }

  bool analog_ = false;
  int sck_ = -1, ws_ = -1, sd_ = -1, adcPin_ = -1;
  int slot_ = 0;  // I2S: 0 = left, 1 = right
  led::LedEngine *engine_ = nullptr;
  volatile bool present_ = false;
};

}  // namespace audio
}  // namespace sb
