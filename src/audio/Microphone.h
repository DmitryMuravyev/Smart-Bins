#pragma once

// The I2S microphone (INMP441) for the music effects (Task 13).
//
// The microphone is optional. begin() starts a small task that first checks
// whether one answers on the I2S pins; if none does, the task frees the I2S
// driver and ends, the LED engine is told there is no microphone, and the
// music effects stay hidden in the UI. If one is found, the task frees the
// I2S as well and waits: only while a music effect runs (and a few seconds
// after) does it read the microphone, every 256 samples (16 ms) through
// AudioAnalyzer, the result handed to the LED engine for the music effects.
// That keeps the driver's and the buffers' memory free the rest of the time.
//
// Wiring (default pin map): SCK GPIO26, WS GPIO25, SD GPIO34, L/R to GND
// (left channel), VDD 3.3 V. Whichever channel turns out to carry sound is
// used, so L/R tied to 3.3 V works as well.

#include <cstdint>

namespace sb {
namespace led {
class LedEngine;
}

namespace audio {

class Microphone {
 public:
  static constexpr uint32_t kSampleRate = 16000;

  // Starts the check (and, with a microphone, the reading) in the background.
  // A pin of -1 means the microphone isn't wired: nothing starts.
  void begin(int sck, int ws, int sd, led::LedEngine &engine);

  // True once a microphone has been found (the check takes about half a
  // second after begin()).
  bool present() const { return present_; }

 private:
  static void taskEntry(void *arg);
  void run();
  // Starts the I2S channel (both slots, 32 bits); nullptr on failure. The
  // handle is a void * so this header needn't pull in the I2S driver.
  void *open();
  void close(void *channel);

  int sck_ = -1, ws_ = -1, sd_ = -1;
  led::LedEngine *engine_ = nullptr;
  volatile bool present_ = false;
};

}  // namespace audio
}  // namespace sb
