#pragma once

// Estimates strip current from the output bytes and scales the frame down when
// it would exceed the configured limit.

#include <cstddef>
#include <cstdint>

namespace sb {
namespace led {

// Current of one colour channel at full value, in tenths of a milliamp.
// Derived from the full-white current per pixel divided by its channels.
struct ChipCurrent {
  uint16_t channelTenthsMa;
  uint8_t idleTenthsMa;  // quiescent current per pixel
};

// Returns the estimated current in milliamps for `pixels` pixels with `channels` bytes each.
inline uint32_t estimateMilliamps(const uint8_t *bytes, size_t pixels, uint8_t channels, ChipCurrent chip) {
  uint64_t sum = 0;
  for (size_t i = 0; i < pixels * channels; i++) sum += bytes[i];
  // sum / 255 full channels x channel current, plus idle current.
  const uint64_t tenths = sum * chip.channelTenthsMa / 255 + static_cast<uint64_t>(pixels) * chip.idleTenthsMa;
  return static_cast<uint32_t>(tenths / 10);
}

// Level (0..255) to apply so `estimateMa` drops to `limitMa`; 255 when within the limit.
inline uint8_t limitLevel(uint32_t estimateMa, uint32_t limitMa) {
  if (estimateMa <= limitMa || estimateMa == 0) return 255;
  return static_cast<uint8_t>(static_cast<uint64_t>(limitMa) * 255 / estimateMa);
}

}  // namespace led
}  // namespace sb
