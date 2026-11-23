#pragma once

// One addressable LED data line driven by the classic ESP32's RMT peripheral:
// the StripOutput of SB_BOARD_ESP32 (see StripOutput.h for the interface
// every board's driver provides).
//
// Unlike FastLED's compile-time controllers, the GPIO, bit timing and output
// polarity are chosen at runtime, so they can come from config.json.

#include <cstddef>
#include <cstdint>

#include <driver/rmt_tx.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "OutputCommon.h"

namespace sb {
namespace led {

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

  // Shares the RMT's transmit memory among the strips about to be started.
  // Call once, before the first begin(), with the number of strips that will
  // be started; each begin() then takes its share (see .cpp for why it
  // matters). Without a plan every strip gets the minimum, one block.
  static void planStrips(int count);

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

}  // namespace led
}  // namespace sb
