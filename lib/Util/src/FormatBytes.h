#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace sb {

// Formats a byte count as a short human-readable string ("512 B", "1.5 KB", "4.00 MB").
// Writes into `out` (always NUL-terminated) and returns it for convenience.
inline const char *formatBytes(uint64_t bytes, char *out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return out;
  }
  static const char *const kUnits[] = {"B", "KB", "MB", "GB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit < 3) {
    value /= 1024.0;
    unit++;
  }
  if (unit == 0) {
    snprintf(out, outSize, "%llu B", static_cast<unsigned long long>(bytes));
  } else if (value < 10.0) {
    snprintf(out, outSize, "%.2f %s", value, kUnits[unit]);
  } else {
    snprintf(out, outSize, "%.1f %s", value, kUnits[unit]);
  }
  return out;
}

}  // namespace sb
