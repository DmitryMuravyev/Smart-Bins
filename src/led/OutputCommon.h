#pragma once

// What every LED output driver shares, whichever board it is for: the bit
// timing of an LED type, the flash guard and the effect clock. The driver
// itself is chosen in StripOutput.h.

#include <cstddef>
#include <cstdint>

namespace sb {
namespace led {

// Bit timing in nanoseconds: high and low time for a 0 bit and a 1 bit.
struct BitTiming {
  uint16_t t0h, t0l, t1h, t1l;
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
// far end of a long strip (for example about 3 s after a light change, when
// the light state is saved).
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
// The driver implements it (RmtStrip.cpp on the classic ESP32), since only
// it knows which frames are on the wire.
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
