#include <unity.h>

#include "Color.h"
#include "Envelope.h"
#include "GridMapper.h"
#include "PowerLimit.h"

using namespace sb::led;

void setUp() {}
void tearDown() {}

static GridGeometry grid(uint8_t cols, uint8_t rows, uint8_t ppb, bool serpentine, bool top, bool left) {
  GridGeometry g;
  g.cols = cols;
  g.rows = rows;
  g.pixelsPerBin = ppb;
  g.serpentine = serpentine;
  g.startTop = top;
  g.startLeft = left;
  return g;
}

static uint16_t first(const GridMapper &m, uint8_t x, uint8_t y) {
  PixelSpan s;
  TEST_ASSERT_TRUE(m.binSpan(x, y, s));
  return s.first;
}

static void test_serpentine_top_left() {
  GridMapper m(grid(10, 6, 2, true, true, true));
  TEST_ASSERT_EQUAL(120, m.segmentLength(0));
  TEST_ASSERT_EQUAL(0, first(m, 0, 0));
  TEST_ASSERT_EQUAL(18, first(m, 9, 0));
  TEST_ASSERT_EQUAL(20, first(m, 9, 1));  // row 1 runs right to left
  TEST_ASSERT_EQUAL(38, first(m, 0, 1));
  TEST_ASSERT_EQUAL(40, first(m, 0, 2));
  TEST_ASSERT_EQUAL(118, first(m, 0, 5));  // last row runs right to left
  PixelSpan s;
  TEST_ASSERT_FALSE(m.binSpan(10, 0, s));
  TEST_ASSERT_FALSE(m.binSpan(0, 6, s));
}

static void test_progressive_bottom_right() {
  GridMapper m(grid(4, 3, 1, false, false, false));
  TEST_ASSERT_EQUAL(0, first(m, 3, 2));  // bottom-right is pixel 0
  TEST_ASSERT_EQUAL(3, first(m, 0, 2));
  TEST_ASSERT_EQUAL(4, first(m, 3, 1));  // every row starts on the right
  TEST_ASSERT_EQUAL(11, first(m, 0, 0));
}

static void test_two_segments() {
  GridGeometry g = grid(10, 6, 2, true, true, true);
  g.segments = 2;
  g.splitRow = 3;
  GridMapper m(g);
  TEST_ASSERT_EQUAL(2, m.segmentCount());
  TEST_ASSERT_EQUAL(60, m.segmentLength(0));
  TEST_ASSERT_EQUAL(60, m.segmentLength(1));
  PixelSpan s;
  TEST_ASSERT_TRUE(m.binSpan(0, 3, s));
  TEST_ASSERT_EQUAL(1, s.segment);
  TEST_ASSERT_EQUAL(0, s.first);  // segment B restarts on the left
  TEST_ASSERT_TRUE(m.binSpan(0, 4, s));
  TEST_ASSERT_EQUAL(38, s.first);
}

static void test_bin_at_inverts_bin_span() {
  GridGeometry layouts[] = {grid(10, 6, 2, true, true, true), grid(7, 5, 3, false, false, true),
                            grid(5, 4, 1, true, false, false)};
  layouts[1].segments = 2;
  layouts[1].splitRow = 2;
  for (const GridGeometry &g : layouts) {
    GridMapper m(g);
    for (uint8_t y = 0; y < g.rows; y++) {
      for (uint8_t x = 0; x < g.cols; x++) {
        PixelSpan s;
        TEST_ASSERT_TRUE(m.binSpan(x, y, s));
        for (uint8_t i = 0; i < s.count; i++) {
          uint8_t bx = 0, by = 0;
          TEST_ASSERT_TRUE(m.binAt(s.segment, s.first + i, bx, by));
          TEST_ASSERT_EQUAL(x, bx);
          TEST_ASSERT_EQUAL(y, by);
        }
      }
    }
  }
}

static void test_scale_and_gamma() {
  TEST_ASSERT_EQUAL(255, scale8(255, 255));
  TEST_ASSERT_EQUAL(0, scale8(200, 0));
  TEST_ASSERT_EQUAL(128, scale8(255, 128));
  Gamma gamma;
  TEST_ASSERT_EQUAL(0, gamma(0));
  TEST_ASSERT_EQUAL(1, gamma(1));  // never rounds a lit value to off
  TEST_ASSERT_EQUAL(255, gamma(255));
  TEST_ASSERT_TRUE(gamma(128) < 70);
  TEST_ASSERT_TRUE(blendMax(Rgb(10, 200, 0), Rgb(50, 20, 0)) == Rgb(50, 200, 0));
}

static void test_envelopes() {
  TEST_ASSERT_EQUAL(0, rampUp(0, 400));
  TEST_ASSERT_EQUAL(127, rampUp(200, 400));
  TEST_ASSERT_EQUAL(255, rampUp(400, 400));
  TEST_ASSERT_EQUAL(255, rampUp(0, 0));
  TEST_ASSERT_EQUAL(255, pulseLevel(PulseMode::Steady, 1234));
  TEST_ASSERT_EQUAL(255, pulseLevel(PulseMode::Slow, 0));
  TEST_ASSERT_EQUAL(kPulseMinLevel, pulseLevel(PulseMode::Slow, kSlowPulsePeriodMs / 2));
  TEST_ASSERT_EQUAL(255, pulseLevel(PulseMode::Fast, kFastPulsePeriodMs));

  Fader f(0);
  f.set(200, 1000, 400);
  TEST_ASSERT_EQUAL(0, f.value(1000));
  TEST_ASSERT_EQUAL(100, f.value(1200));
  TEST_ASSERT_EQUAL(200, f.value(1400));
  f.set(0, 1400, 400);
  TEST_ASSERT_EQUAL(100, f.value(1600));
  f.set(255, 1600, 400);  // reversing mid-way starts from the current value, no jump
  TEST_ASSERT_EQUAL(100, f.value(1600));
  TEST_ASSERT_FALSE(f.settled(1900));
  TEST_ASSERT_TRUE(f.settled(2000));
}

static void test_power_limit() {
  uint8_t white[3 * 10];
  for (uint8_t &b : white) b = 255;
  const ChipCurrent ws2812b{200, 10};  // 20 mA per channel, 1 mA idle
  TEST_ASSERT_EQUAL(610, estimateMilliamps(white, 10, 3, ws2812b));
  TEST_ASSERT_EQUAL(255, limitLevel(610, 1000));
  TEST_ASSERT_EQUAL(127, limitLevel(1000, 500));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_serpentine_top_left);
  RUN_TEST(test_progressive_bottom_right);
  RUN_TEST(test_two_segments);
  RUN_TEST(test_bin_at_inverts_bin_span);
  RUN_TEST(test_scale_and_gamma);
  RUN_TEST(test_envelopes);
  RUN_TEST(test_power_limit);
  return UNITY_END();
}
