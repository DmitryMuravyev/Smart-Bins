#pragma once

// Default pin map for the ESP32-DevKitC (ESP32-WROOM-32) and the rules used to
// validate any pin map, including one edited later from the web UI.
// See docs/hardware/README.md for the circuits behind each function.

#include <cstddef>
#include <cstdint>

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

// Priority 1: addressable bin grid. Priority 2: workspace lights.
// Priority 3: auxiliary lights and microphone. Then optional extras.
constexpr PinAssignment kDefaultPins[] = {
    {"grid_data_a", 16, PinUse::Output, true},
    {"grid_data_b", 17, PinUse::Output, true},
    {"grid_power", 32, PinUse::Output, true},
    {"light_w1", 27, PinUse::Output, true},  // W1-W3: universal (white PWM, analog RGB or addressable)
    {"light_w2", 13, PinUse::Output, true},
    {"light_w3", 4, PinUse::Output, true},
    {"light_power", 33, PinUse::Output, true},  // power switch for addressable work strips
    {"light_w4", 21, PinUse::Output, true},  // W4-W5: white PWM only
    {"light_w5", 22, PinUse::Output, true},
    {"mic_i2s_sck", 26, PinUse::Output, false},
    {"mic_i2s_ws", 25, PinUse::Output, false},
    {"mic_i2s_sd", 34, PinUse::Input, false},
    {"mic_analog", kNoPin, PinUse::Input, false},  // GPIO35 for an analog microphone instead of I2S
    {"sd_sck", 18, PinUse::Output, false},
    {"sd_miso", 19, PinUse::Input, false},
    {"sd_mosi", 23, PinUse::Output, false},
    {"sd_cs", 5, PinUse::Output, false},
    {"button", 0, PinUse::Input, false},
    {"status_led", 2, PinUse::Output, true},
};

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

static_assert(defaultPin("grid_data_a") == 16, "findPin works at compile time");

// GPIO numbers that exist on the ESP32 package used by the WROOM-32 module.
constexpr bool gpioExists(int gpio) {
  return gpio >= 0 && gpio <= 39 && gpio != 20 && gpio != 24 && !(gpio >= 28 && gpio <= 31);
}

// Wired to the module's internal SPI flash; touching them crashes the chip.
constexpr bool isFlashPin(int gpio) { return gpio >= 6 && gpio <= 11; }

// No output driver and no internal pull-ups.
constexpr bool isInputOnly(int gpio) { return gpio >= 34 && gpio <= 39; }

// USB serial console (boot log, flashing).
constexpr bool isConsolePin(int gpio) { return gpio == 1 || gpio == 3; }

// GPIO12 selects the flash voltage at reset; pulling it high on a WROOM-32 stops it booting.
constexpr bool isForbiddenStrapping(int gpio) { return gpio == 12; }

enum class PinError : uint8_t {
  None,
  NoSuchGpio,
  FlashPin,
  ConsolePin,
  StrappingPin,
  InputOnlyUsedAsOutput,
  Duplicate,
};

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

constexpr const char *pinErrorText(PinError e) {
  switch (e) {
    case PinError::None: return "ok";
    case PinError::NoSuchGpio: return "no such GPIO on ESP32-WROOM-32";
    case PinError::FlashPin: return "GPIO6-11 are used by the internal flash";
    case PinError::ConsolePin: return "GPIO1/3 are the USB serial console";
    case PinError::StrappingPin: return "GPIO12 sets flash voltage at boot";
    case PinError::InputOnlyUsedAsOutput: return "GPIO34-39 are input-only";
    case PinError::Duplicate: return "GPIO already used by another function";
  }
  return "unknown";
}

static_assert(validatePinMap(kDefaultPins, kDefaultPinCount).error == PinError::None,
              "default pin map is invalid");

}  // namespace board
}  // namespace sb
