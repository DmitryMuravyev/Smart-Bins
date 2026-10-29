#pragma once

// One addressable LED data line driven by the ESP32's RMT peripheral.
//
// Unlike FastLED's compile-time controllers, the GPIO, bit timing and output
// polarity are chosen at runtime, so they can come from config.json.

#include <cstddef>
#include <cstdint>

#include <driver/rmt_tx.h>

namespace sb {
namespace led {

// Bit timing in nanoseconds: high and low time for a 0 bit and a 1 bit.
struct BitTiming {
  uint16_t t0h, t0l, t1h, t1l;
};

class RmtStrip {
 public:
  RmtStrip() = default;
  RmtStrip(const RmtStrip &) = delete;
  RmtStrip &operator=(const RmtStrip &) = delete;
  ~RmtStrip() { end(); }

  // `invert` flips the output for an inverting (single-transistor) level shifter;
  // the line then idles high at the pin and low at the strip.
  bool begin(int gpio, const BitTiming &timing, bool invert);
  void end();

  // Starts sending `len` bytes. `data` must stay unchanged until the next call
  // to write() or waitDone(), which wait for the previous frame to finish.
  bool write(const uint8_t *data, size_t len);
  bool waitDone(uint32_t timeoutMs);
  bool ready() const { return channel_ != nullptr; }

 private:
  rmt_channel_handle_t channel_ = nullptr;
  rmt_encoder_handle_t encoder_ = nullptr;
  bool busy_ = false;
};

}  // namespace led
}  // namespace sb
