#pragma once

// Sound analysis for the music effects: turns blocks of microphone
// samples into what the effects draw from, 16 frequency bands, the overall
// loudness and a beat counter.
//
// Per block of kN samples (16 ms at 16 kHz, so about 62 blocks a second):
//   1. the DC offset is removed and a Hann window applied;
//   2. an FFT gives the spectrum; its bins are summed into 16 bands spaced
//      evenly on a logarithmic (musical) scale, 60 Hz to 8 kHz, each in dB;
//   3. auto-gain: the loudest band of recent blocks is tracked (it jumps up at
//      once and sinks slowly, a few dB per second), so the effects scale to
//      whatever is playing, quiet or loud, without a volume knob;
//   4. beats: the bass energy (about 40..160 Hz) is compared with its own
//      recent average; a sudden rise well above it counts as a beat.
//
// Levels are in dB relative to full scale: a full-scale sine reads about
// 0 dB, the INMP441's own noise about -85 dB, speech across a room about
// -65 dB and music at living-room level about -45 dB. The analyzer has no
// noise gate of its own: the effects decide what is "too quiet to show"
// (their intensity setting), so one slider covers both the sensitivity and
// the look.
//
// Nothing here touches hardware or reads a clock: blocks come from the
// microphone task on the board and from generated signals in the host tests.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

namespace sb {
namespace led {

// What the music effects get, refreshed with every block.
struct AudioFrame {
  static constexpr int kBands = 16;
  float bandDb[kBands];  // low to high
  float levelDb;         // loudness of the whole block (RMS)
  float peakDb;          // auto-gain reference: recent loudest band
  float levelPeakDb;     // auto-gain reference for levelDb
  float bassDb;          // energy of the bass range
  float bassPeakDb;      // recent loudest bass
  uint32_t beats;        // counts up by one on every beat

  AudioFrame() { clear(); }
  // Silence: what the effects see without a microphone or when it stops.
  void clear() {
    for (float &b : bandDb) b = kSilenceDb;
    levelDb = peakDb = levelPeakDb = bassDb = bassPeakDb = kSilenceDb;
    beats = 0;
  }
  static constexpr float kSilenceDb = -120.0f;
};

class AudioAnalyzer {
 public:
  static constexpr int kN = 256;  // samples per block (a power of two for the FFT)
  static constexpr int kBands = AudioFrame::kBands;
  static constexpr float kLowHz = 60.0f, kHighHz = 8000.0f;
  // Auto-gain release: how fast the reference sinks after loud passages.
  static constexpr float kPeakFallDbPerS = 3.0f;
  // Beats: the bass has to reach this many times its recent average (1.8 is
  // about +2.5 dB), be within kBeatRangeDb of the recent loudest bass and
  // kBeatShareDb of the whole sound, and not come sooner than kBeatGapMs
  // after the last one (240 BPM at most).
  static constexpr float kBeatRatio = 1.8f;
  static constexpr float kBeatRangeDb = 15.0f;
  static constexpr float kBeatFloorDb = -80.0f;
  static constexpr float kBeatShareDb = 10.0f;  // bass within this of the whole sound's level
  static constexpr uint32_t kBeatGapMs = 250;

  explicit AudioAnalyzer(uint32_t sampleRate = 16000) : sampleRate_(sampleRate) {
    // Band edges in FFT bins, logarithmically spaced. On a 256-point FFT the
    // lowest bands would be narrower than one bin; each band gets at least
    // one, which pushes the low edges up a little and keeps them in order.
    const float binHz = static_cast<float>(sampleRate) / kN;
    const int maxBin = kN / 2;
    int prev = 0;
    for (int b = 0; b <= kBands; b++) {
      const float hz = kLowHz * std::pow(kHighHz / kLowHz, static_cast<float>(b) / kBands);
      int bin = static_cast<int>(std::lround(hz / binHz));
      if (bin < 1) bin = 1;
      if (b > 0 && bin <= prev) bin = prev + 1;
      if (bin > maxBin) bin = maxBin;
      edges_[b] = static_cast<uint8_t>(bin > 255 ? 255 : bin);
      prev = bin;
    }
    bassLo_ = static_cast<int>(std::lround(40.0f / binHz));
    bassHi_ = static_cast<int>(std::lround(160.0f / binHz));
    if (bassLo_ < 1) bassLo_ = 1;
    if (bassHi_ < bassLo_) bassHi_ = bassLo_;
    blockMs_ = 1000.0f * kN / sampleRate;
  }

  // First and last+1 FFT bin of band b (for the tests).
  int bandStart(int b) const { return edges_[b]; }
  int bandEnd(int b) const { return edges_[b + 1]; }
  uint32_t sampleRate() const { return sampleRate_; }

  // Analyses one block of kN samples, full scale = +-1.0.
  const AudioFrame &process(const float *samples) {
    // DC offset and loudness.
    float mean = 0;
    for (int i = 0; i < kN; i++) mean += samples[i];
    mean /= kN;
    float sumSq = 0;
    // The Hann window, 0.5 - 0.5 cos(2 pi i / N), worked out as it goes
    // instead of kept in a table (1 KB less): (c, d) is the point at angle
    // 2 pi i / N on the unit circle, turned one step further per sample.
    const float stepC = std::cos(2.0f * kPi / kN), stepS = std::sin(2.0f * kPi / kN);
    float c = 1, d = 0;
    for (int i = 0; i < kN; i++) {
      const float s = samples[i] - mean;
      sumSq += s * s;
      re_[i] = s * (0.5f - 0.5f * c);
      im_[i] = 0;
      const float nc = c * stepC - d * stepS;
      d = c * stepS + d * stepC;
      c = nc;
    }
    // RMS in dB; +3 dB so a full-scale sine (RMS 0.707) reads 0 dB like the bands.
    frame_.levelDb = toDb(sumSq / kN) + 3.01f;

    fft(re_, im_);

    // Power per bin, scaled so a full-scale sine's main lobe sums to about
    // 0 dB: the Hann window halves the amplitude, the FFT multiplies it by N/2.
    const float norm = 4.0f / kN;
    float maxBand = AudioFrame::kSilenceDb;
    for (int b = 0; b < kBands; b++) {
      float p = 0;
      for (int k = edges_[b]; k < edges_[b + 1]; k++) p += power(k, norm);
      frame_.bandDb[b] = toDb(p);
      if (frame_.bandDb[b] > maxBand) maxBand = frame_.bandDb[b];
    }
    float bass = 0;
    for (int k = bassLo_; k <= bassHi_; k++) bass += power(k, norm);
    frame_.bassDb = toDb(bass);

    // Auto-gain references: up at once, down slowly.
    const float fall = kPeakFallDbPerS * blockMs_ / 1000.0f;
    frame_.peakDb = follow(frame_.peakDb, maxBand, fall);
    frame_.levelPeakDb = follow(frame_.levelPeakDb, frame_.levelDb, fall);
    frame_.bassPeakDb = follow(frame_.bassPeakDb, frame_.bassDb, fall);

    // Beats.
    sinceBeatMs_ += blockMs_;
    const bool rising = bass > kBeatRatio * bassAvg_;
    // The bass also has to be a real part of the sound, not the low end of
    // hiss or a hum, whose few bins swing wildly from block to block.
    const bool loudEnough = frame_.bassDb > frame_.bassPeakDb - kBeatRangeDb && frame_.bassDb > kBeatFloorDb &&
                            frame_.bassDb > frame_.levelDb - kBeatShareDb;
    if (rising && loudEnough && sinceBeatMs_ >= kBeatGapMs) {
      frame_.beats++;
      sinceBeatMs_ = 0;
    }
    // The average covers about the last half second (1/32 per block at 62 a
    // second), long enough to hold a level between kicks, short enough to
    // follow a song getting louder.
    bassAvg_ += (bass - bassAvg_) / 32.0f;
    return frame_;
  }

  const AudioFrame &frame() const { return frame_; }

 private:
  static constexpr float kPi = 3.14159265358979f;

  float power(int k, float norm) const {
    const float r = re_[k] * norm, i = im_[k] * norm;
    return r * r + i * i;
  }

  static float toDb(float power) {
    return power > 1e-12f ? 10.0f * std::log10(power) : AudioFrame::kSilenceDb;
  }

  static float follow(float ref, float value, float fall) {
    if (value >= ref) return value;
    const float lower = ref - fall;
    return lower > value ? lower : value;
  }

  // In-place iterative radix-2 FFT (bit-reversal, then butterflies).
  static void fft(float *re, float *im) {
    for (int i = 1, j = 0; i < kN; i++) {
      int bit = kN >> 1;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) {
        std::swap(re[i], re[j]);
        std::swap(im[i], im[j]);
      }
    }
    for (int len = 2; len <= kN; len <<= 1) {
      const float ang = -2.0f * kPi / len;
      const float wr = std::cos(ang), wi = std::sin(ang);
      for (int i = 0; i < kN; i += len) {
        float cr = 1, ci = 0;
        for (int k = 0; k < len / 2; k++) {
          const int a = i + k, b = i + k + len / 2;
          const float tr = re[b] * cr - im[b] * ci;
          const float ti = re[b] * ci + im[b] * cr;
          re[b] = re[a] - tr;
          im[b] = im[a] - ti;
          re[a] += tr;
          im[a] += ti;
          const float ncr = cr * wr - ci * wi;
          ci = cr * wi + ci * wr;
          cr = ncr;
        }
      }
    }
  }

  uint32_t sampleRate_;
  float blockMs_ = 16;
  float re_[kN], im_[kN];
  uint8_t edges_[kBands + 1];
  int bassLo_ = 1, bassHi_ = 2;
  float bassAvg_ = 0;
  float sinceBeatMs_ = kBeatGapMs;  // the very first kick counts
  AudioFrame frame_;
};

}  // namespace led
}  // namespace sb
