#include <cstdlib>
#include <unity.h>

#include "Color.h"
#include "Envelope.h"
#include "GridEffects.h"
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
  TEST_ASSERT_EQUAL(0, gamma(1));   // the darkest inputs round to off, no stuck "1" tail
  TEST_ASSERT_EQUAL(1, gamma(20));
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
  // A time read just before set() (another task) must not count as "finished".
  TEST_ASSERT_EQUAL(100, f.value(1599));
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

// ---- Grid effects (Task 12) ----

static void test_effect_and_palette_names() {
  for (int i = 0; i < kGridEffectCount; i++) {
    GridEffect e;
    TEST_ASSERT_TRUE(parseGridEffect(toString(static_cast<GridEffect>(i)), e));
    TEST_ASSERT_EQUAL(i, static_cast<int>(e));
  }
  for (int i = 0; i < kPaletteCount; i++) {
    Palette p;
    TEST_ASSERT_TRUE(parsePalette(toString(static_cast<Palette>(i)), p));
    TEST_ASSERT_EQUAL(i, static_cast<int>(p));
  }
  GridEffect e;
  Palette p;
  TEST_ASSERT_FALSE(parseGridEffect("lasers", e));
  TEST_ASSERT_FALSE(parsePalette(nullptr, p));
}

static void test_palette_ends() {
  // A non-wrapping palette runs exactly from its first stop to its last.
  TEST_ASSERT_TRUE(paletteColor(Palette::Heat, 0) == Rgb(0, 0, 0));
  TEST_ASSERT_TRUE(paletteColor(Palette::Heat, 255) == Rgb(255, 255, 200));
  // A wrapping one comes back round to its first colour.
  TEST_ASSERT_TRUE(paletteColor(Palette::Rainbow, 0) == Rgb(255, 0, 0));
  TEST_ASSERT_TRUE(paletteColor(Palette::Rainbow, 255) == Rgb(255, 0, 0));
  // Neighbouring indices never jump: at most a sixth of the way between two stops.
  for (int p = 1; p < kPaletteCount; p++) {
    for (int i = 0; i < 255; i++) {
      const Rgb a = paletteColor(static_cast<Palette>(p), i), b = paletteColor(static_cast<Palette>(p), i + 1);
      TEST_ASSERT_TRUE(abs(a.r - b.r) <= 12 && abs(a.g - b.g) <= 12 && abs(a.b - b.b) <= 12);
    }
  }
}

static int level(Rgb c) { return c.r + c.g + c.b; }

static void test_fire_burns_from_the_bottom() {
  EffectPlayer player;
  player.configure(10, 6);
  player.start(GridEffect::Fire, 0, 42);
  Rgb out[60];
  long top = 0, bottom = 0;
  for (uint32_t t = 2000; t < 8000; t += 16) {
    player.render(t, t, out);
    for (int x = 0; x < 10; x++) {
      top += level(out[x]);
      bottom += level(out[50 + x]);
    }
  }
  TEST_ASSERT_TRUE(bottom > 3 * top);
  TEST_ASSERT_TRUE(top > 0);  // flames do reach the top now and then
}

static void test_fire_intensity_sets_the_height() {
  long lit[2] = {0, 0};
  const uint8_t intensities[2] = {30, 230};
  for (int k = 0; k < 2; k++) {
    EffectPlayer player;
    player.configure(10, 6);
    EffectParams p;
    p.intensity = intensities[k];
    player.setParams(p, 0);
    player.start(GridEffect::Fire, 0, 7);
    Rgb out[60];
    for (uint32_t t = 2000; t < 6000; t += 16) {
      player.render(t, t, out);
      for (int i = 0; i < 40; i++) lit[k] += level(out[i]);  // the upper four rows
    }
  }
  TEST_ASSERT_TRUE(lit[1] > 4 * lit[0]);
}

static void test_effects_are_repeatable() {
  // Same seed, same frames: a picture can be reproduced in a test.
  for (GridEffect e : {GridEffect::Fire, GridEffect::Sparks, GridEffect::Relax}) {
    EffectPlayer a, b;
    a.configure(10, 6);
    b.configure(10, 6);
    a.start(e, 0, 99);
    b.start(e, 0, 99);
    Rgb oa[60], ob[60];
    for (uint32_t t = 0; t < 3000; t += 16) {
      a.render(t, 5, oa);
      b.render(t, 5, ob);
    }
    TEST_ASSERT_EQUAL_MEMORY(oa, ob, sizeof(oa));
  }
}

static void test_sparks_come_and_go() {
  EffectPlayer player;
  player.configure(10, 6);
  player.start(GridEffect::Sparks, 0, 3);
  Rgb out[60];
  int changes = 0, litFrames = 0;
  Rgb prev[60] = {};
  for (uint32_t t = 2000; t < 6000; t += 16) {
    player.render(t, t, out);
    int lit = 0;
    for (int i = 0; i < 60; i++) {
      if (!out[i].isBlack()) lit++;
      if (!(out[i] == prev[i])) changes++;
      prev[i] = out[i];
    }
    if (lit > 0 && lit < 60) litFrames++;
  }
  TEST_ASSERT_TRUE(changes > 1000);
  TEST_ASSERT_TRUE(litFrames > 200);  // some bins lit, never the whole grid
}

static void test_player_fades_in_and_out() {
  EffectPlayer player;
  player.configure(10, 6);
  Rgb out[60];
  TEST_ASSERT_FALSE(player.active());
  TEST_ASSERT_FALSE(player.render(0, 1, out));
  player.start(GridEffect::Relax, 1000, 1);
  TEST_ASSERT_TRUE(player.active());
  player.render(1000, 1, out);
  TEST_ASSERT_EQUAL(0, level(out[0]) + level(out[30]));  // starts from dark
  player.render(1000 + EffectPlayer::kFadeMs / 2, 1, out);
  const int half = level(out[30]);
  player.render(1000 + EffectPlayer::kFadeMs + 100, 1, out);
  const int full = level(out[30]);
  TEST_ASSERT_TRUE(half > 0 && half < full);
  player.start(GridEffect::Off, 5000, 1);
  TEST_ASSERT_TRUE(player.active());  // fading out
  player.render(5000 + EffectPlayer::kFadeMs / 2, 1, out);
  TEST_ASSERT_TRUE(level(out[30]) > 0);
  player.render(5000 + EffectPlayer::kFadeMs + 1, 1, out);
  TEST_ASSERT_FALSE(player.active());
  TEST_ASSERT_FALSE(player.render(5000 + EffectPlayer::kFadeMs + 20, 1, out));
}

static void test_player_timer_and_demo() {
  EffectPlayer player;
  player.configure(10, 6);
  EffectParams p;
  p.timerMin = 2;
  player.setParams(p, 0);
  player.start(GridEffect::Demo, 0, 1);
  Rgb out[60];
  TEST_ASSERT_EQUAL(120, player.remainingS(0));
  TEST_ASSERT_EQUAL(GridEffect::Demo, player.selected());
  // The demo moves on every kDemoMs; render often enough to notice.
  for (uint32_t t = 0; t < 119000; t += 50) player.render(t, t, out);
  TEST_ASSERT_EQUAL(1, player.remainingS(119000));
  TEST_ASSERT_TRUE(player.active());
  player.render(120000, 1, out);
  TEST_ASSERT_EQUAL(GridEffect::Off, player.selected());  // the timer stopped it
  TEST_ASSERT_EQUAL(0, player.remainingS(120000));
  player.render(120000 + EffectPlayer::kFadeMs, 1, out);
  TEST_ASSERT_FALSE(player.active());
}

static void test_player_brightness_and_speed_zero() {
  EffectPlayer player;
  player.configure(4, 3);
  EffectParams p;
  p.speed = 0;  // treated as the slowest speed, never a division by zero
  p.brightness = 0;
  player.setParams(p, 0);
  player.start(GridEffect::Fire, 0, 5);
  Rgb out[12];
  for (uint32_t t = 0; t < 4000; t += 16) player.render(t, t, out);
  for (const Rgb &c : out) TEST_ASSERT_TRUE(c.isBlack());
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
  RUN_TEST(test_effect_and_palette_names);
  RUN_TEST(test_palette_ends);
  RUN_TEST(test_fire_burns_from_the_bottom);
  RUN_TEST(test_fire_intensity_sets_the_height);
  RUN_TEST(test_effects_are_repeatable);
  RUN_TEST(test_sparks_come_and_go);
  RUN_TEST(test_player_fades_in_and_out);
  RUN_TEST(test_player_timer_and_demo);
  RUN_TEST(test_player_brightness_and_speed_zero);
  return UNITY_END();
}
