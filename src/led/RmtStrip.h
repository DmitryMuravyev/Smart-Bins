#pragma once

// One addressable LED data line driven by the ESP32's RMT peripheral.
//
// Unlike FastLED's compile-time controllers, the GPIO, bit timing and output
// polarity are chosen at runtime, so they can come from config.json.

#include <cstddef>
#include <cstdint>

#include <driver/rmt_tx.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

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
  friend class FlashGuard;
  static bool onDone(rmt_channel_handle_t, const rmt_tx_done_event_data_t *, void *ctx);

  rmt_channel_handle_t channel_ = nullptr;
  rmt_encoder_handle_t encoder_ = nullptr;
  bool busy_ = false;
  // True from rmt_transmit() until the done interrupt; read by FlashGuard,
  // which can't use rmt_tx_wait_all_done() from another task (see .cpp).
  volatile bool inFlight_ = false;
};

// Pauses every strip around a write to flash (NVS / Preferences, LittleFS).
//
// Writing flash switches off the CPU's flash cache for a few milliseconds, and
// with it every interrupt handler that lives in flash, including the RMT one
// that keeps refilling the transmit buffer. A frame that is on the wire at
// that moment stops half way: the strip latches what it got and the rest of
// the pixels take the next bits as a new frame, which shows as a flash on the
// far end of a long strip (seen about 3 s after every light change, when the
// light state was saved).
//
// While a FlashGuard exists, write() waits instead of starting a frame, and
// the constructor returns only once frames already on the wire have finished:
//
//   { led::FlashGuard guard; prefs.putBytes(...); }
//
// Strip tasks just pause for the length of the write (a missed frame or two).
class FlashGuard {
 public:
  FlashGuard();
  ~FlashGuard();
  FlashGuard(const FlashGuard &) = delete;
  FlashGuard &operator=(const FlashGuard &) = delete;
};

}  // namespace led
}  // namespace sb
