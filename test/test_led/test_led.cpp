#include <unity.h>

#include "Color.h"
#include "Envelope.h"
#include "GridMapper.h"
#include "PowerLimit.h"
#include "WorkEffects.h"

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

static void test_fader16() {
  Fader16 f(0);
  f.set(65535, 0, 5000);  // 65535 x 5000 overflows int32: checks the 64-bit maths
  TEST_ASSERT_EQUAL(32767, f.value(2500));
  TEST_ASSERT_EQUAL(65535, f.value(5000));
  f.reset(100);
  TEST_ASSERT_EQUAL(100, f.value(0));
  TEST_ASSERT_TRUE(f.settled(0));
}

static void test_hue_and_kelvin() {
  TEST_ASSERT_TRUE(hueToRgb(0) == Rgb(255, 0, 0));
  TEST_ASSERT_TRUE(hueToRgb(85) == Rgb(1, 255, 0));    // a hair before pure green
  TEST_ASSERT_TRUE(hueToRgb(128) == Rgb(0, 255, 255));  // cyan, opposite red
  // Yellow keeps both channels at full value (the six-sector wheel).
  const Rgb y = hueToRgb(42);
  TEST_ASSERT_EQUAL(255, y.r);
  TEST_ASSERT_GREATER_THAN(240, y.g);

  const Rgb warm = kelvinToRgb(2700);
  TEST_ASSERT_EQUAL(255, warm.r);
  TEST_ASSERT_LESS_THAN(warm.g, warm.b);  // warm light has less blue than green
  const Rgb day = kelvinToRgb(6600);
  TEST_ASSERT_GREATER_THAN(245, day.g);
  TEST_ASSERT_GREATER_THAN(245, day.b);
  TEST_ASSERT_TRUE(kelvinToRgb(100) == kelvinToRgb(kMinKelvin));  // clamped
}

static void test_work_effects() {
  const Rgb base(200, 100, 50);
  TEST_ASSERT_TRUE(effectPixel(WorkEffect::Solid, base, 3, 10, 1234) == base);
  TEST_ASSERT_TRUE(effectPixel(WorkEffect::Breathe, base, 0, 1, 0) == base);  // starts at full level
  const Rgb low = effectPixel(WorkEffect::Breathe, base, 0, 1, kBreathePeriodMs / 2);
  TEST_ASSERT_EQUAL(scale8(200, kBreatheMinLevel), low.r);
  // Rainbow: one full wheel along the strip, so pixel 0 and pixel n/2 are opposite hues.
  TEST_ASSERT_TRUE(effectPixel(WorkEffect::Rainbow, base, 0, 6, 0) == Rgb(255, 0, 0));
  TEST_ASSERT_TRUE(effectPixel(WorkEffect::Rainbow, base, 3, 6, 0) == hueToRgb(128));
  // ...and it turns with time.
  TEST_ASSERT_TRUE(effectPixel(WorkEffect::Rainbow, base, 0, 1, kRainbowCycleMs / 2) == hueToRgb(128));

  WorkEffect e;
  TEST_ASSERT_TRUE(parseWorkEffect("breathe", e));
  TEST_ASSERT_TRUE(e == WorkEffect::Breathe);
  TEST_ASSERT_FALSE(parseWorkEffect("fire", e));
  TEST_ASSERT_TRUE(lerp(Rgb(0, 0, 0), Rgb(255, 100, 0), 255) == Rgb(255, 100, 0));
  TEST_ASSERT_TRUE(lerp(Rgb(0, 0, 0), Rgb(255, 100, 0), 0) == Rgb(0, 0, 0));
}

static void test_pwm_duty() {
  TEST_ASSERT_EQUAL(0, pwmDuty(0, 4095));
  TEST_ASSERT_EQUAL(1, pwmDuty(1, 4095));  // never stuck at 0 while fading in
  TEST_ASSERT_EQUAL(4095, pwmDuty(65535, 4095));
  // Half perceived brightness is about 22 % duty with gamma 2.2.
  const uint16_t half = pwmDuty(32768, 4095);
  TEST_ASSERT_INT_WITHIN(10, 896, half);
  // Monotonic: a fade never steps backwards.
  uint16_t prev = 0;
  for (uint32_t v = 0; v <= 65535; v += 97) {
    const uint16_t d = pwmDuty(static_cast<uint16_t>(v), 4095);
    TEST_ASSERT_TRUE(d >= prev);
    prev = d;
  }
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
  RUN_TEST(test_fader16);
  RUN_TEST(test_hue_and_kelvin);
  RUN_TEST(test_work_effects);
  RUN_TEST(test_pwm_duty);
  return UNITY_END();
}
