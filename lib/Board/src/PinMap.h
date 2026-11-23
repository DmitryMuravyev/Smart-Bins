#pragma once

// Pin map: the board's default GPIO for every function and the rules used to
// validate any pin map, including one edited later from the web UI. The
// board-specific parts (the default map, which GPIOs exist and which are
// reserved) are in boards/<board>Pins.h, picked by SB_BOARD (BoardProfile.h).

#include <cstddef>
#include <cstdint>

#include "BoardProfile.h"

namespace sb {
namespace board {

constexpr int8_t kNoPin = -1;

enum class PinUse : uint8_t {
  Output,  // driven by the ESP32
  Input,   // read only
};

struct PinAssignment {
  const char *name;
  int8_t gpio;  // kNoPin when the function is not wired
  PinUse use;
  bool lowAtBoot;  // drive LOW as early as possible so LEDs stay dark during boot
};

enum class PinError : uint8_t {
  None,
  NoSuchGpio,
  FlashPin,
  ConsolePin,
  StrappingPin,
  PsramPin,
  InputOnlyUsedAsOutput,
  Duplicate,
};

// The board's default pin map (kDefaultPins), its GPIO rules (gpioExists,
// isFlashPin, isInputOnly, isConsolePin, isForbiddenStrapping, isPsramPin)
// and pinErrorText().
#if SB_BOARD == SB_BOARD_ESP32
#include "boards/Esp32Pins.h"
#endif

constexpr size_t kDefaultPinCount = sizeof(kDefaultPins) / sizeof(kDefaultPins[0]);

// GPIO assigned to a named function in a pin map, or kNoPin.
constexpr int8_t findPin(const PinAssignment *pins, size_t count, const char *name) {
  for (size_t i = 0; i < count; i++) {
    const char *a = pins[i].name;
    const char *b = name;
    while (*a && *a == *b) {
      a++;
      b++;
    }
    if (*a == *b) return pins[i].gpio;
  }
  return kNoPin;
}

constexpr int8_t defaultPin(const char *name) { return findPin(kDefaultPins, kDefaultPinCount, name); }

// Position of a named function in kDefaultPins (and in every pin map, which
// keeps the same order), or -1.
constexpr int pinIndex(const char *name) {
  for (size_t i = 0; i < kDefaultPinCount; i++) {
    const char *a = kDefaultPins[i].name;
    const char *b = name;
    while (*a && *a == *b) {
      a++;
      b++;
    }
    if (*a == *b) return static_cast<int>(i);
  }
  return -1;
}

// The BOOT button can't be moved: holding it is how a bad pin map (or any
// other setting) is reset, so it has to work whatever the configuration says.
constexpr bool isFixedPin(const char *name) { return pinIndex(name) == pinIndex("button"); }

static_assert(defaultPin(kDefaultPins[0].name) == kDefaultPins[0].gpio, "findPin works at compile time");

struct PinCheck {
  PinError error;
  size_t index;  // entry that failed
  size_t other;  // first entry using the same GPIO, for Duplicate
};

constexpr PinCheck checkPin(const PinAssignment &pin) {
  const int g = pin.gpio;
  if (g == kNoPin) return {PinError::None, 0, 0};
  if (!gpioExists(g)) return {PinError::NoSuchGpio, 0, 0};
  if (isFlashPin(g)) return {PinError::FlashPin, 0, 0};
  if (isConsolePin(g)) return {PinError::ConsolePin, 0, 0};
  if (isForbiddenStrapping(g)) return {PinError::StrappingPin, 0, 0};
  if (isPsramPin(g)) return {PinError::PsramPin, 0, 0};
  if (pin.use == PinUse::Output && isInputOnly(g)) return {PinError::InputOnlyUsedAsOutput, 0, 0};
  return {PinError::None, 0, 0};
}

// Returns the first problem found in a pin map, or PinError::None.
constexpr PinCheck validatePinMap(const PinAssignment *pins, size_t count) {
  for (size_t i = 0; i < count; i++) {
    PinCheck c = checkPin(pins[i]);
    if (c.error != PinError::None) return {c.error, i, i};
    for (size_t j = 0; j < i; j++) {
      if (pins[i].gpio != kNoPin && pins[i].gpio == pins[j].gpio) return {PinError::Duplicate, i, j};
    }
  }
  return {PinError::None, 0, 0};
}

static_assert(validatePinMap(kDefaultPins, kDefaultPinCount).error == PinError::None,
              "default pin map is invalid");

}  // namespace board
}  // namespace sb
