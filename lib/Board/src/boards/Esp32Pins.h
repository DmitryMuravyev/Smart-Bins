#pragma once

// Classic ESP32 (ESP32-WROOM-32 / -32E / -32UE on an ESP32-DevKitC): default
// pin map, the GPIO rules for this chip and their messages. Included by
// PinMap.h when SB_BOARD is SB_BOARD_ESP32; see BoardProfile.h.
// docs/hardware/esp32-wroom/README.md has the circuits behind each function.
//
// Inside namespace sb::board; PinMap.h defines PinAssignment, PinUse, kNoPin
// and PinError before including this file.

// Priority 1: addressable bin grid. Priority 2: workspace lights.
// Priority 3: auxiliary lights and microphone. Then optional extras.
constexpr PinAssignment kDefaultPins[] = {
    // Grid data on GPIO14/15, keeping GPIO16/17 free for PSRAM (modules
    // such as the ESP32-WROOM-32E-N16R2 wire their PSRAM there).
    {"grid_data_a", 14, PinUse::Output, true},
    {"grid_data_b", 15, PinUse::Output, true},
    {"grid_power", 32, PinUse::Output, true},
    {"light_w1", 27, PinUse::Output, true},  // W1-W5: white PWM or addressable; W1-W3 also analog RGB
    {"light_w2", 13, PinUse::Output, true},
    {"light_w3", 4, PinUse::Output, true},
    {"light_power", 33, PinUse::Output, true},  // power switch for addressable work strips
    {"light_w4", 21, PinUse::Output, true},
    {"light_w5", 22, PinUse::Output, true},
    {"mic_i2s_sck", 26, PinUse::Output, false},
    {"mic_i2s_ws", 25, PinUse::Output, false},
    {"mic_i2s_sd", 34, PinUse::Input, false},
    {"mic_analog", 35, PinUse::Input, false},  // analog microphone (MAX9814), used instead of I2S when chosen; ADC1 only
    {"sd_sck", 18, PinUse::Output, false},
    {"sd_miso", 19, PinUse::Input, false},
    {"sd_mosi", 23, PinUse::Output, false},
    {"sd_cs", 5, PinUse::Output, false},
    {"button", 0, PinUse::Input, false},
    {"status_led", 2, PinUse::Output, true},
    // LED supply remote control (EPP-300-12 and similar, hardware guide
    // section 7): PS-ON switches its main output, PG reports it is up. Not
    // wired by default; the guide uses GPIO2 (status LED then set to none)
    // and GPIO39.
    {"ps_on", kNoPin, PinUse::Output, true},
    {"power_good", kNoPin, PinUse::Input, false},
};

// Highest GPIO number on the chip; config values above it are refused.
constexpr int kMaxGpio = 39;

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

// PSRAM on the classic ESP32 uses GPIO16 (CS) and GPIO17 (CLK); on a module
// with PSRAM (the PSRAM build, SB_PSRAM) they are not available. Without
// PSRAM they are ordinary pins.
#if defined(SB_PSRAM) && SB_PSRAM
constexpr bool isPsramPin(int gpio) { return gpio == 16 || gpio == 17; }
#else
constexpr bool isPsramPin(int) { return false; }
#endif

// Message for each PinError, worded for this chip.
constexpr const char *pinErrorText(PinError e) {
  switch (e) {
    case PinError::None: return "ok";
    case PinError::NoSuchGpio: return "no such GPIO on ESP32-WROOM-32";
    case PinError::FlashPin: return "GPIO6-11 are used by the internal flash";
    case PinError::ConsolePin: return "GPIO1/3 are the USB serial console";
    case PinError::StrappingPin: return "GPIO12 sets flash voltage at boot";
    case PinError::PsramPin: return "GPIO16/17 are used by the PSRAM";
    case PinError::InputOnlyUsedAsOutput: return "GPIO34-39 are input-only";
    case PinError::Duplicate: return "GPIO already used by another function";
  }
  return "unknown";
}

