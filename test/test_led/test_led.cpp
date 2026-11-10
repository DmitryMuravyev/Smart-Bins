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

// A value keeps its level for as long as the board runs: the millisecond
// clock passing half its range (24.9 days) or wrapping (49.7 days) after a
// fade must not bring the old value back.
static void test_fader_survives_clock_wrap() {
  const uint32_t day = 24u * 60 * 60 * 1000;
  // Sampled along the way, as a light that stays on is.
  Fader16 light(0);
  light.set(4095, 1000, 400);
  TEST_ASSERT_EQUAL(4095, light.value(2000));
  TEST_ASSERT_EQUAL(4095, light.value(1000 + 25 * day));
  TEST_ASSERT_EQUAL(4095, light.value(1000 + 49 * day));
  TEST_ASSERT_EQUAL(4095, light.value(500));  // 49.7 days on: the clock has wrapped
  TEST_ASSERT_EQUAL(4095, light.value(1200));
  TEST_ASSERT_TRUE(light.settled(1200));
  // Not sampled since the change (a brightness set while the grid is dark).
  Fader level(128);
  level.set(40, 1000, 400);
  TEST_ASSERT_EQUAL(40, level.value(1000 + 25 * day));
  // A new change after the wrap fades as usual.
  light.set(0, 2000, 400);
  TEST_ASSERT_EQUAL(4095, light.value(2000));
  TEST_ASSERT_EQUAL(0, light.value(2400));
  // A fade that starts just before the wrap and ends after it.
  Fader f(0);
  f.set(200, UINT32_MAX - 199, 400);
  TEST_ASSERT_EQUAL(0, f.value(UINT32_MAX - 199));
  TEST_ASSERT_EQUAL(100, f.value(0));
  TEST_ASSERT_EQUAL(200, f.value(200));
}

static void test_power_limit() {
  uint8_t white[3 * 10];
  for (uint8_t &b : white) b = 255;
  const ChipCurrent ws2812b{200, 10};  // 20 mA per channel, 1 mA idle
  TEST_ASSERT_EQUAL(610, estimateMilliamps(white, 10, 3, ws2812b));
  TEST_ASSERT_EQUAL(255, limitLevel(610, 1000));
  TEST_ASSERT_EQUAL(127, limitLevel(1000, 500));
}

// A measured figure per pixel replaces the datasheet one: full white then
// estimates exactly that.
static void test_measured_pixel_current() {
  const ChipCurrent ws2812b{200, 10};
  TEST_ASSERT_EQUAL(61, fullWhiteMa(ws2812b, 3));
  const ChipCurrent measured = withFullWhiteMa(ws2812b, 3, 17);
  TEST_ASSERT_EQUAL(53, measured.channelTenthsMa);  // (170 - 10) / 3
  TEST_ASSERT_EQUAL(10, measured.idleTenthsMa);
  uint8_t white[3 * 150];
  for (uint8_t &b : white) b = 255;
  TEST_ASSERT_UINT32_WITHIN(150, 2550, estimateMilliamps(white, 150, 3, measured));
  // RGBW: full white lights the white LED alone.
  const ChipCurrent rgbw{200, 10};
  TEST_ASSERT_EQUAL(21, fullWhiteMa(rgbw, 4));
  TEST_ASSERT_EQUAL(140, withFullWhiteMa(rgbw, 4, 15).channelTenthsMa);
  // Below the idle current nothing is left for the LEDs.
  TEST_ASSERT_EQUAL(0, withFullWhiteMa(ChipCurrent{200, 20}, 3, 1).channelTenthsMa);
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

// ---- Grid effects ----

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
  // A wrapping one comes back round to its first color.
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

// ---- Music ----

// One block of a sine at `hz`, amplitude `amp`, continuing from sample `t`.
static void sineBlock(float *out, float hz, float amp, long t, uint32_t rate = 16000) {
  for (int i = 0; i < AudioAnalyzer::kN; i++) out[i] = amp * std::sin(2.0f * 3.14159265f * hz * (t + i) / rate);
}

static void test_analyzer_finds_the_band() {
  AudioAnalyzer a;
  // Bands are in order, every one at least a bin wide, and cover 60 Hz..8 kHz.
  for (int b = 0; b < AudioFrame::kBands; b++) TEST_ASSERT_TRUE(a.bandEnd(b) > a.bandStart(b));
  TEST_ASSERT_EQUAL(1, a.bandStart(0));
  TEST_ASSERT_EQUAL(AudioAnalyzer::kN / 2, a.bandEnd(AudioFrame::kBands - 1));
  float s[AudioAnalyzer::kN];
  const float tones[] = {100, 440, 1000, 3000, 7000};
  for (float hz : tones) {
    AudioAnalyzer an;
    const AudioFrame *f = nullptr;
    for (int blk = 0; blk < 3; blk++) {
      sineBlock(s, hz, 0.5f, blk * AudioAnalyzer::kN);
      f = &an.process(s);
    }
    // The loudest band is the one holding the tone, and stands well above the rest.
    const int bin = static_cast<int>(hz * AudioAnalyzer::kN / 16000 + 0.5f);
    int loudest = 0;
    for (int b = 1; b < AudioFrame::kBands; b++)
      if (f->bandDb[b] > f->bandDb[loudest]) loudest = b;
    TEST_ASSERT_TRUE(bin >= an.bandStart(loudest) - 1 && bin <= an.bandEnd(loudest));
    // Half scale: about -6 dB, in the band and overall.
    TEST_ASSERT_FLOAT_WITHIN(2.5f, -6.0f, f->bandDb[loudest]);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, -6.0f, f->levelDb);
  }
}

static void test_analyzer_auto_gain() {
  AudioAnalyzer a;
  float s[AudioAnalyzer::kN];
  long t = 0;
  // Loud music sets the reference at once...
  for (int blk = 0; blk < 10; blk++, t += AudioAnalyzer::kN) {
    sineBlock(s, 1000, 0.5f, t);
    a.process(s);
  }
  TEST_ASSERT_FLOAT_WITHIN(1.0f, a.frame().levelDb, a.frame().levelPeakDb);
  // ...and after it turns 20 dB quieter the reference follows within ~8 s
  // (3 dB a second), so the effects fill the grid again.
  for (int blk = 0; blk < 62 * 3; blk++, t += AudioAnalyzer::kN) {
    sineBlock(s, 1000, 0.05f, t);
    a.process(s);
  }
  TEST_ASSERT_TRUE(a.frame().levelPeakDb - a.frame().levelDb > 8.0f);  // still sinking
  for (int blk = 0; blk < 62 * 6; blk++, t += AudioAnalyzer::kN) {
    sineBlock(s, 1000, 0.05f, t);
    a.process(s);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.5f, a.frame().levelDb, a.frame().levelPeakDb);
  // Silence reads as silence.
  for (float &v : s) v = 0;
  a.process(s);
  TEST_ASSERT_TRUE(a.frame().levelDb <= -100.0f);
}

// 120 BPM: a decaying 70 Hz kick every 0.5 s over a steady tone and some noise.
static int countBeats(bool kicks, float seconds) {
  AudioAnalyzer a;
  Random rnd(7);
  float s[AudioAnalyzer::kN];
  long t = 0;
  const long blocks = static_cast<long>(seconds * 16000 / AudioAnalyzer::kN);
  for (long blk = 0; blk < blocks; blk++) {
    for (int i = 0; i < AudioAnalyzer::kN; i++, t++) {
      const float ts = (t % 8000) / 16000.0f;
      const float kick = kicks && ts < 0.12f ? 0.5f * std::exp(-ts * 25) * std::sin(2 * 3.14159265f * 70 * ts) : 0;
      const float noise = 0.05f * (rnd.below(2001) / 1000.0f - 1.0f);
      s[i] = kick + noise + 0.1f * std::sin(2 * 3.14159265f * 440 * t / 16000.0f);
    }
    a.process(s);
  }
  return static_cast<int>(a.frame().beats);
}

static void test_analyzer_counts_beats() {
  TEST_ASSERT_EQUAL(16, countBeats(true, 8.0f));  // kicks at 0, 0.5 .. 7.5 s
  TEST_ASSERT_EQUAL(0, countBeats(false, 8.0f));  // a tone and hiss are no beat
}

// A frame with every band, the loudness and the bass at `db`, the auto-gain
// references at `refDb`.
static AudioFrame soundAt(float db, float refDb, uint32_t beats = 0) {
  AudioFrame f;
  for (float &b : f.bandDb) b = db;
  f.levelDb = f.bassDb = db;
  f.peakDb = f.levelPeakDb = f.bassPeakDb = refDb;
  f.beats = beats;
  return f;
}

static int litCells(const Rgb *out, int n) {
  int lit = 0;
  for (int i = 0; i < n; i++) lit += level(out[i]) > 30;
  return lit;
}

static void test_music_names() {
  GridEffect e;
  TEST_ASSERT_TRUE(parseGridEffect("spectrum", e) && e == GridEffect::Spectrum);
  TEST_ASSERT_TRUE(parseGridEffect("pulse", e) && e == GridEffect::Pulse);
  TEST_ASSERT_EQUAL_STRING("beat", toString(GridEffect::Beat));
  TEST_ASSERT_TRUE(isMusicEffect(GridEffect::Beat));
  TEST_ASSERT_FALSE(isMusicEffect(GridEffect::Demo));
}

static void test_spectrum_bars() {
  EffectPlayer player;
  player.configure(10, 6);
  player.start(GridEffect::Spectrum, 0, 1);
  Rgb out[60];
  // Loud bass, quiet treble: the left columns stand tall, the right ones stay dark.
  AudioFrame f = soundAt(-80, -30);
  for (int b = 0; b < 4; b++) f.bandDb[b] = -30;
  for (int b = 4; b < 8; b++) f.bandDb[b] = -45;  // half way into the 30 dB window
  player.setAudio(f);
  for (uint32_t t = 0; t <= 2000; t += 16) player.render(t, 1, out);
  for (int y = 0; y < 6; y++) TEST_ASSERT_TRUE(level(out[y * 10 + 0]) > 30);  // full bar
  TEST_ASSERT_TRUE(level(out[0 * 10 + 4]) == 0 && level(out[5 * 10 + 4]) > 30);  // half bar: bottom only
  TEST_ASSERT_TRUE(level(out[3 * 10 + 4]) > 30 && level(out[2 * 10 + 4]) == 0);
  for (int y = 0; y < 6; y++) TEST_ASSERT_EQUAL(0, level(out[y * 10 + 9]));
  // Colors by height, like a level meter: green at the bottom, red at the top.
  const Rgb bottom = out[5 * 10 + 0], top = out[0 * 10 + 0];
  TEST_ASSERT_TRUE(bottom.g > bottom.r && bottom.b == 0);
  TEST_ASSERT_TRUE(top.r > top.g && top.b == 0);
  // When the sound stops, the bars sink over a moment rather than vanish.
  player.setAudio(soundAt(-120, -30));
  player.render(2016, 1, out);
  TEST_ASSERT_TRUE(level(out[1 * 10 + 0]) > 30);
  for (uint32_t t = 2032; t <= 3000; t += 16) player.render(t, 1, out);
  TEST_ASSERT_EQUAL(0, litCells(out, 60));
}

static void test_music_gate_follows_intensity() {
  // Room noise at -70 dB: dark at the default sensitivity, showing at the highest.
  for (int pass = 0; pass < 2; pass++) {
    EffectPlayer player;
    player.configure(10, 6);
    EffectParams p;
    p.intensity = pass ? 255 : 128;
    player.setParams(p, 0);
    player.start(GridEffect::Pulse, 0, 1);
    player.setAudio(soundAt(-70, -70));
    Rgb out[60];
    for (uint32_t t = 0; t <= 2000; t += 16) player.render(t, 1, out);
    if (pass) TEST_ASSERT_TRUE(litCells(out, 60) > 0);
    else TEST_ASSERT_EQUAL(0, litCells(out, 60));
  }
}

static void test_pulse_grows_with_loudness() {
  EffectPlayer player;
  player.configure(10, 6);
  player.start(GridEffect::Pulse, 0, 1);
  Rgb out[60];
  int lit[3];
  const float levels[3] = {-52, -42, -30};  // quiet, medium, loudest
  uint32_t t = 0;
  for (int i = 0; i < 3; i++) {
    player.setAudio(soundAt(levels[i], -30));
    for (uint32_t end = t + 2000; t <= end; t += 16) player.render(t, 1, out);
    lit[i] = litCells(out, 60);
  }
  TEST_ASSERT_TRUE(lit[0] > 0 && lit[0] < lit[1] && lit[1] < lit[2]);
  TEST_ASSERT_EQUAL(60, lit[2]);
  // The middle is lit before the corners.
  player.setAudio(soundAt(-48, -30));
  for (uint32_t end = t + 2000; t <= end; t += 16) player.render(t, 1, out);
  TEST_ASSERT_TRUE(level(out[2 * 10 + 4]) > 30);
  TEST_ASSERT_EQUAL(0, level(out[0]));
}

static void test_beat_splashes() {
  EffectPlayer player;
  player.configure(10, 6);
  // A beat counted before the effect started doesn't flash.
  player.setAudio(soundAt(-30, -30, 5));
  player.start(GridEffect::Beat, 0, 3);
  Rgb out[60];
  for (uint32_t t = 0; t <= 2000; t += 16) player.render(t, 1, out);
  TEST_ASSERT_EQUAL(0, litCells(out, 60));
  // A new beat lights a splash, which fades within a second or so.
  player.setAudio(soundAt(-30, -30, 6));
  player.render(2016, 1, out);
  const int splash = litCells(out, 60);
  TEST_ASSERT_TRUE(splash >= 5);
  uint32_t t = 2016;
  for (; t <= 2500; t += 16) player.render(t, 1, out);
  TEST_ASSERT_TRUE(litCells(out, 60) <= splash);
  for (; t <= 3600; t += 16) player.render(t, 1, out);
  TEST_ASSERT_EQUAL(0, litCells(out, 60));
  // A beat too quiet for the gate doesn't flash.
  player.setAudio(soundAt(-95, -95, 7));
  player.render(t, 1, out);
  TEST_ASSERT_EQUAL(0, litCells(out, 60));
}

static void test_player_settings_per_effect() {
  // Relax at full brightness, then sparks at a low one: while relax fades
  // out it keeps its own brightness (and timer-free settings) instead of
  // dropping to the new effect's; sparks then runs with its own.
  EffectPlayer player;
  player.configure(10, 6);
  EffectParams relax, sparks;
  relax.brightness = 255;
  sparks.brightness = 20;
  sparks.intensity = 255;
  sparks.timerMin = 3;
  Rgb out[60];
  player.start(GridEffect::Relax, relax, 0, 1);
  for (uint32_t t = 0; t <= 3000; t += 16) player.render(t, 1, out);
  int before = 0;
  for (const Rgb &c : out) before += level(c);
  player.start(GridEffect::Sparks, sparks, 3000, 2);
  TEST_ASSERT_EQUAL(20, player.params().brightness);
  TEST_ASSERT_EQUAL(180, player.remainingS(3000));  // the new effect's timer
  player.render(3016, 1, out);
  int during = 0;
  for (const Rgb &c : out) during += level(c);
  TEST_ASSERT_TRUE(during > before * 8 / 10);  // relax still nearly at full brightness
  for (uint32_t t = 3016; t <= 6000; t += 16) player.render(t, 1, out);
  for (const Rgb &c : out) TEST_ASSERT_TRUE(c.r <= 20 && c.g <= 20 && c.b <= 20);
  // Off keeps the settings: the fade-out uses them, and nothing else changes.
  player.start(GridEffect::Off, EffectParams(), 6000, 3);
  TEST_ASSERT_EQUAL(20, player.params().brightness);
}

// Settings changes fade (issue: a slider let go made the picture jump).
static void test_player_settings_fade() {
  EffectPlayer player;
  player.configure(10, 6);
  player.setParamFadeMs(400);
  EffectParams p;
  p.brightness = 255;
  player.start(GridEffect::Relax, p, 0, 1);
  Rgb out[60];
  for (uint32_t t = 0; t <= 3000; t += 16) player.render(t, 1, out);
  int full = 0;
  for (const Rgb &c : out) full += level(c);
  p.brightness = 25;
  player.setParams(p, 3000);
  TEST_ASSERT_EQUAL(25, player.params().brightness);  // the target, at once
  // Relax changes slowly, so frames a few ms apart compare fairly.
  player.render(3000, 1, out);
  int start = 0;
  for (const Rgb &c : out) start += level(c);
  player.render(3200, 1, out);
  int half = 0;
  for (const Rgb &c : out) half += level(c);
  player.render(3400, 1, out);
  int end = 0;
  for (const Rgb &c : out) end += level(c);
  TEST_ASSERT_TRUE(start > full * 9 / 10);           // no jump at the change
  TEST_ASSERT_TRUE(half < start * 8 / 10 && half > end * 2);  // part way
  TEST_ASSERT_TRUE(end < full * 15 / 100);           // there after the fade
  // A change during a fade starts from where it had got to.
  p.brightness = 255;
  player.setParams(p, 3500);
  player.render(3500, 1, out);
  int again = 0;
  for (const Rgb &c : out) again += level(c);
  TEST_ASSERT_TRUE(again < full * 15 / 100);
  // With no fade time, changes apply at once.
  player.setParamFadeMs(0);
  p.brightness = 25;
  player.setParams(p, 4000);
  player.render(4000, 1, out);
  int instant = 0;
  for (const Rgb &c : out) instant += level(c);
  TEST_ASSERT_TRUE(instant < full * 15 / 100);
}

// A new palette cross-fades, and rendering the old colors along the way
// doesn't disturb the effect: once the fade is over, the picture is exactly
// the one of an effect that had the new palette all along.
// Settings changed long ago stay as set when the clock passes half its range.
static void test_player_settings_survive_clock_wrap() {
  EffectPlayer player;
  player.configure(10, 6);
  player.setParamFadeMs(400);
  EffectParams p;
  p.brightness = 255;
  player.start(GridEffect::Relax, p, 0, 1);
  Rgb out[60];
  for (uint32_t t = 0; t <= 3000; t += 16) player.render(t, 1, out);
  int full = 0;
  for (const Rgb &c : out) full += level(c);
  p.brightness = 25;
  player.setParams(p, 3000);
  for (uint32_t t = 3000; t <= 4000; t += 16) player.render(t, 1, out);
  const uint32_t later = 4000 + 25u * 24 * 60 * 60 * 1000;
  player.render(later, 1, out);
  int dim = 0;
  for (const Rgb &c : out) dim += level(c);
  TEST_ASSERT_TRUE(dim < full * 15 / 100);
}

static void test_player_palette_fade() {
  EffectParams party, ocean;
  party.palette = Palette::Party;
  ocean.palette = Palette::Ocean;
  EffectPlayer a, b;
  a.configure(10, 6);
  b.configure(10, 6);
  a.start(GridEffect::Sparks, party, 0, 42);
  b.start(GridEffect::Sparks, ocean, 0, 42);
  Rgb oa[60], ob[60];
  for (uint32_t t = 0; t <= 2000; t += 16) {
    a.render(t, 5, oa);
    b.render(t, 5, ob);
  }
  a.setParams(ocean, 2000);
  int differ = 0;
  for (uint32_t t = 2016; t <= 2400; t += 16) {
    a.render(t, 5, oa);
    b.render(t, 5, ob);
    differ += memcmp(oa, ob, sizeof(oa)) != 0;
  }
  TEST_ASSERT_TRUE(differ > 0);  // the old colors were still mixed in
  for (uint32_t t = 2416; t <= 4000; t += 16) {
    a.render(t, 5, oa);
    b.render(t, 5, ob);
  }
  TEST_ASSERT_EQUAL_MEMORY(ob, oa, sizeof(oa));
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
  RUN_TEST(test_measured_pixel_current);
  RUN_TEST(test_fader_survives_clock_wrap);
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
  RUN_TEST(test_analyzer_finds_the_band);
  RUN_TEST(test_analyzer_auto_gain);
  RUN_TEST(test_analyzer_counts_beats);
  RUN_TEST(test_music_names);
  RUN_TEST(test_spectrum_bars);
  RUN_TEST(test_music_gate_follows_intensity);
  RUN_TEST(test_pulse_grows_with_loudness);
  RUN_TEST(test_beat_splashes);
  RUN_TEST(test_player_settings_per_effect);
  RUN_TEST(test_player_settings_fade);
  RUN_TEST(test_player_settings_survive_clock_wrap);
  RUN_TEST(test_player_palette_fade);
  return UNITY_END();
}
