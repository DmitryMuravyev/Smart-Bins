#pragma once

// Which board the firmware is built for, and what differs between boards.
//
// The build picks one with SB_BOARD (platformio.ini sets it for every
// environment); without it, as in the host tests, it is the classic ESP32.
// Everything that depends on the chip goes through this header and the files
// it selects, so a second board (the ESP32-S3 is the planned one) adds its own
// files next to the classic ESP32's and the rest of the code stays shared:
//
//   - the default pin map and the pin rules: boards/<board>Pins.h (PinMap.h)
//   - the LED output driver: src/led/StripOutput.h
//   - the memory limits: MemoryProfile.h (SB_PSRAM, set per module)
//   - the hardware guide: docs/hardware/<board>/
//
// The board's id is stored in config.json and in backups. A config written on
// another board keeps every setting but its pin map, which is reset to this
// board's defaults (see AppConfig.cpp), since the GPIOs mean different things
// there.

#define SB_BOARD_ESP32 1    // ESP32-WROOM-32 / -32E / -32UE on an ESP32-DevKitC
#define SB_BOARD_ESP32S3 2  // reserved for an ESP32-S3 board; not supported yet

#ifndef SB_BOARD
#define SB_BOARD SB_BOARD_ESP32
#endif

#if SB_BOARD != SB_BOARD_ESP32
#error "SB_BOARD: only SB_BOARD_ESP32 is supported so far (see BoardProfile.h)"
#endif

namespace sb {
namespace board {

#if SB_BOARD == SB_BOARD_ESP32
// Short id written to config.json and backups ("board").
constexpr const char *kBoardId = "esp32";
// Human-readable name for messages.
constexpr const char *kBoardName = "ESP32-WROOM-32";
// Addressable LED data lines the output driver can run at once: the classic
// ESP32 has 8 RMT transmit channels, and one strip takes two when it gets the
// extra transmit memory (see RmtStrip.cpp), so 7 (2 grid lines + W1-W5).
constexpr int kMaxStripOutputs = 7;
#endif

}  // namespace board
}  // namespace sb
