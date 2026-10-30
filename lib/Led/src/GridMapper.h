#pragma once

// Maps a bin at (x, y) to its pixels on the strip.
//
// Coordinates are as seen from the front: x = 0 is the left column, y = 0 the
// top row. The strip starts at `start` and runs row by row; with serpentine
// wiring every second row runs back the other way. With two segments, rows
// from `splitRow` on (in wiring order) are on data line B, which starts again
// from the same side as line A.

#include <cstdint>

namespace sb {
namespace led {

struct GridGeometry {
  uint8_t cols = 1;
  uint8_t rows = 1;
  uint8_t pixelsPerBin = 1;
  bool serpentine = true;
  bool startTop = true;
  bool startLeft = true;
  uint8_t segments = 1;
  uint8_t splitRow = 0;  // first wiring row on segment B (only with 2 segments)
};

struct PixelSpan {
  uint8_t segment = 0;
  uint16_t first = 0;  // index of the bin's first pixel on its segment
  uint8_t count = 0;
};

class GridMapper {
 public:
  GridMapper() = default;
  explicit GridMapper(const GridGeometry &g) : g_(g) {}

  const GridGeometry &geometry() const { return g_; }
  uint8_t segmentCount() const { return g_.segments >= 2 ? 2 : 1; }
  uint16_t binCount() const { return static_cast<uint16_t>(g_.cols) * g_.rows; }

  // Rows of the grid on a segment, and its length in pixels.
  uint8_t segmentRows(uint8_t segment) const {
    if (segmentCount() == 1) return segment == 0 ? g_.rows : 0;
    return segment == 0 ? g_.splitRow : static_cast<uint8_t>(g_.rows - g_.splitRow);
  }
  uint16_t segmentLength(uint8_t segment) const {
    return static_cast<uint16_t>(segmentRows(segment)) * g_.cols * g_.pixelsPerBin;
  }

  // Returns false for coordinates outside the grid.
  bool binSpan(uint8_t x, uint8_t y, PixelSpan &out) const {
    if (x >= g_.cols || y >= g_.rows) return false;
    // 1. Row in wiring order: counted from the corner where the strip starts.
    // 2. Which data line: rows from splitRow on belong to segment B, which
    //    restarts its own count (and its own serpentine direction) from 0.
    // 3. Direction of this row: the start side, flipped on every odd row of
    //    a serpentine layout because the strip doubles back.
    // 4. Pixel index = whole rows before it + bins before it in this row,
    //    times pixels per bin.
    const int wiringRow = g_.startTop ? y : g_.rows - 1 - y;
    const bool onB = segmentCount() == 2 && wiringRow >= g_.splitRow;
    const int rowInSegment = onB ? wiringRow - g_.splitRow : wiringRow;
    bool leftToRight = g_.startLeft;
    if (g_.serpentine && (rowInSegment & 1)) leftToRight = !leftToRight;
    const int col = leftToRight ? x : g_.cols - 1 - x;
    out.segment = onB ? 1 : 0;
    out.first = static_cast<uint16_t>((rowInSegment * g_.cols + col) * g_.pixelsPerBin);
    out.count = g_.pixelsPerBin;
    return true;
  }

  // Inverse of binSpan: which bin owns a pixel. Returns false past the segment end.
  // Undoes the steps of binSpan in reverse order (pixel -> bin in segment ->
  // row and column in wiring order -> front-view coordinates).
  bool binAt(uint8_t segment, uint16_t pixel, uint8_t &x, uint8_t &y) const {
    if (segment >= segmentCount() || pixel >= segmentLength(segment)) return false;
    const int binIndex = pixel / g_.pixelsPerBin;
    const int rowInSegment = binIndex / g_.cols;
    const int col = binIndex % g_.cols;
    bool leftToRight = g_.startLeft;
    if (g_.serpentine && (rowInSegment & 1)) leftToRight = !leftToRight;
    x = static_cast<uint8_t>(leftToRight ? col : g_.cols - 1 - col);
    const int wiringRow = rowInSegment + (segment == 1 ? g_.splitRow : 0);
    y = static_cast<uint8_t>(g_.startTop ? wiringRow : g_.rows - 1 - wiringRow);
    return true;
  }

 private:
  GridGeometry g_;
};

}  // namespace led
}  // namespace sb
