#pragma once

// One addressable LED data line driven by the ESP32's RMT peripheral.
//
// Unlike FastLED's compile-time controllers, the GPIO, bit timing and output
// polarity are chosen at runtime, so they can come from config.json.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <driver/rmt_tx.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace sb {
namespace led {

// Allocates from internal RAM. The RMT's refill interrupt reads a frame's
// bytes while it is on the wire, and since the transmit interrupt is built to
// run while the flash cache is off (CONFIG_RMT_TX_ISR_CACHE_SAFE in
// platformio.ini), the bytes must be where the CPU can read them without the
// cache: internal RAM, never PSRAM (which goes through the same cache).
// Without PSRAM every allocation is internal anyway.
template <typename T>
struct InternalAllocator {
  using value_type = T;
  InternalAllocator() = default;
  template <typename U>
  InternalAllocator(const InternalAllocator<U> &) {}
  T *allocate(size_t n) {
    void *p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    // Out of internal RAM: any RAM rather than none. The strip still works;
    // only a flash access by the system (outside a FlashGuard) could then
    // cut a frame short, as before the interrupt was made cache-safe.
    if (!p) p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_8BIT);
    return static_cast<T *>(p);
  }
  void deallocate(T *p, size_t) { heap_caps_free(p); }
  template <typename U>
  bool operator==(const InternalAllocator<U> &) const { return true; }
  template <typename U>
  bool operator!=(const InternalAllocator<U> &) const { return false; }
};

// The encoded bytes of one frame, as handed to RmtStrip::write().
using FrameBytes = std::vector<uint8_t, InternalAllocator<uint8_t>>;

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

// Pauses every strip around a read or write of flash (NVS / Preferences,
// the FAT partition with the config, database and photos).
//
// On the ESP32, reading the flash chip directly (as the file system does)
// switches the cache off just as writing does, only for a shorter time: a
// search reads a few hundred pages of the database, and with frames going
// out meanwhile the strips flashed in random colors. Reads of code and
// constants through the cache are not affected; only file and NVS access.
//
// Writing flash switches off the CPU's flash cache for a few milliseconds, and
// with it every interrupt handler that lives in flash, including the RMT one
// that keeps refilling the transmit buffer. A frame that is on the wire at
// that moment stops half way: the strip latches what it got and the rest of
// the pixels take the next bits as a new frame, which shows as a flash on the
// far end of a long strip (seen about 3 s after every light change, when the
// light state was saved).
//
// Since the transmit interrupt is cache-safe (CONFIG_RMT_TX_ISR_CACHE_SAFE in
// platformio.ini), a frame on the wire no longer stops during a flash access,
// so this guard is no longer what keeps frames whole. It stays as a second
// line of defence (a frame buffer that had to go to PSRAM, a build without
// the option) and for the effect clock below, which keeps effects from
// jumping ahead after a long write.
//
// While a FlashGuard exists, write() waits instead of starting a frame, and
// the constructor returns only once frames already on the wire have finished:
//
//   { led::FlashGuard guard; prefs.putBytes(...); }
//
// Strip tasks just pause for the length of the write (a missed frame or two).
//
// Guards may nest on one task (a write inside a read, say); only the
// outermost one pauses and restarts the strips.
class FlashGuard {
 public:
  FlashGuard();
  ~FlashGuard();
  // The same without a scope, for code that can't hold an object across the
  // write (the database's write hooks). Every lock() needs one unlock() on the
  // same task: the gate is a mutex, owned by the task that took it.
  static void lock();
  static void unlock();
  FlashGuard(const FlashGuard &) = delete;
  FlashGuard &operator=(const FlashGuard &) = delete;
};

// The clock every light effect runs on: milliseconds like millis(), except
// that it stands still while a FlashGuard is held. Strips can't send frames
// during a flash write anyway; with effects timed by millis() they would
// jump ahead by the length of the write when it ends (a pulse skipping part
// of its cycle, a fade suddenly further along). On this clock they pause and
// carry on from the same point. Any task may call it.
uint32_t effectMillis();

}  // namespace led
}  // namespace sb
