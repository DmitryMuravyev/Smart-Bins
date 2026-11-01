#include "audio/Microphone.h"

// Arduino.h first: it routes ESP_LOGx through Arduino's logger, which prints
// at the build's CORE_DEBUG_LEVEL. Without it these are the IDF's own macros,
// which the prebuilt core keeps at errors only, and the info lines vanish.
#include <Arduino.h>

#include <cmath>
#include <cstdint>
#include <new>

#include <driver/i2s_std.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "AudioAnalyzer.h"
#include "led/LedEngine.h"

namespace sb {
namespace audio {

namespace {

const char *const TAG = "mic";

// DMA: 3 buffers of 128 frames (8 ms each at 16 kHz), 3 KB in all. The task
// reads one buffer at a time; three give it 16 ms of slack before samples
// are lost, which only happens during flash writes (when the I2S interrupt
// has to wait) and costs at most a block of analysis.
constexpr uint32_t kDmaBuffers = 3;
constexpr uint32_t kDmaFrames = 128;

// The INMP441 needs 2^18 clock cycles after the clock starts before its
// output is valid (about 260 ms at 16 kHz x 64); the check skips that and
// then listens for a while.
constexpr uint32_t kSettleMs = 300;
constexpr uint32_t kListenMs = 250;

// The work buffers, allocated only while the task runs (about 4.5 KB):
// one DMA buffer's worth of raw stereo frames, a block of mono samples for
// the analyzer, and the analyzer itself.
struct Work {
  int32_t raw[kDmaFrames * 2];
  float block[led::AudioAnalyzer::kN];
  led::AudioAnalyzer analyzer{Microphone::kSampleRate};
};

// A raw I2S word to a sample: the INMP441 sends 24 bits, left-aligned in
// the 32-bit slot.
inline float toSample(int32_t raw) { return static_cast<float>(raw >> 8) / 8388608.0f; }

// What the check learns about one channel (left or right slot).
struct SlotStats {
  int32_t min = INT32_MAX, max = INT32_MIN;
  double sumSq = 0;
  uint32_t n = 0;

  void add(int32_t raw) {
    const int32_t v = raw >> 8;
    if (v < min) min = v;
    if (v > max) max = v;
    const double s = v / 8388608.0;
    sumSq += s * s;
    n++;
  }
  float rmsDb() const { return n && sumSq > 0 ? 10.0f * std::log10(static_cast<float>(sumSq / n)) : -120.0f; }
  // A microphone's output always moves a little (its own noise is a few
  // hundred steps of 24 bits), while a channel nobody drives reads one
  // value over and over. A level near full scale is not sound either but
  // a floating data line picking up the clock.
  bool live() const { return n && max - min >= 8 && rmsDb() < -10.0f; }
};

}  // namespace

void Microphone::begin(int sck, int ws, int sd, led::LedEngine &engine) {
  if (sck < 0 || ws < 0 || sd < 0) {
    ESP_LOGI(TAG, "Not wired (no pins assigned)");
    return;
  }
  sck_ = sck;
  ws_ = ws;
  sd_ = sd;
  engine_ = &engine;
  // 4 KB of stack: the log lines with floating-point numbers alone take
  // over 1 KB, and with 3 KB only about 500 bytes were left unused.
  // Core 1 with the LED task, below it in priority: the analysis takes well
  // under a millisecond per 16 ms block, and core 0 can be busy for a long
  // time with a database query in the web server, which would drop audio.
  xTaskCreatePinnedToCore(taskEntry, "mic", 4096, this, 1, nullptr, 1);
}

void Microphone::taskEntry(void *arg) {
  static_cast<Microphone *>(arg)->run();
  vTaskDelete(nullptr);
}

void Microphone::run() {
  Work *work = new (std::nothrow) Work;
  if (!work) {
    ESP_LOGE(TAG, "Not enough memory for the microphone");
    return;
  }

  i2s_chan_handle_t rx = nullptr;
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num = kDmaBuffers;
  chanCfg.dma_frame_num = kDmaFrames;
  // Both slots are read: the check finds out which one the microphone uses.
  i2s_std_config_t stdCfg = {};
  stdCfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
  stdCfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  stdCfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  stdCfg.gpio_cfg.bclk = static_cast<gpio_num_t>(sck_);
  stdCfg.gpio_cfg.ws = static_cast<gpio_num_t>(ws_);
  stdCfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  stdCfg.gpio_cfg.din = static_cast<gpio_num_t>(sd_);

  esp_err_t err = i2s_new_channel(&chanCfg, nullptr, &rx);
  if (err == ESP_OK) err = i2s_channel_init_std_mode(rx, &stdCfg);
  if (err == ESP_OK) err = i2s_channel_enable(rx);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "I2S start failed: %s", esp_err_to_name(err));
    if (rx) i2s_del_channel(rx);
    delete work;
    return;
  }

  // ---- Is anything there? ----
  SlotStats slots[2];
  const uint32_t chunkMs = 1000 * kDmaFrames / kSampleRate;
  for (uint32_t ms = 0; ms < kSettleMs + kListenMs; ms += chunkMs) {
    size_t got = 0;
    if (i2s_channel_read(rx, work->raw, sizeof(work->raw), &got, pdMS_TO_TICKS(100)) != ESP_OK) continue;
    if (ms < kSettleMs) continue;
    for (size_t i = 0; i + 1 < got / sizeof(int32_t); i += 2) {
      slots[0].add(work->raw[i]);
      slots[1].add(work->raw[i + 1]);
    }
  }
  // Both channels in the log, for checking the wiring: the microphone's
  // channel should move a little in a quiet room (-60..-85 dB), the other
  // one should not move at all.
  for (int i = 0; i < 2; i++) {
    ESP_LOGI(TAG, "I2S %s channel: %.0f dB, samples %ld..%ld", i ? "right" : "left", slots[i].rmsDb(),
             (long)slots[i].min, (long)slots[i].max);
  }
  const int slot = slots[0].live() ? 0 : slots[1].live() ? 1 : -1;
  if (slot < 0) {
    ESP_LOGI(TAG, "No microphone on GPIO %d/%d/%d; music effects off", sck_, ws_, sd_);
    i2s_channel_disable(rx);
    i2s_del_channel(rx);
    delete work;
    return;
  }
  ESP_LOGI(TAG, "Microphone found (%s channel, room level %.0f dB)", slot ? "right" : "left", slots[slot].rmsDb());
  present_ = true;
  engine_->setMicrophone(true);

  // ---- Listen ----
  int filled = 0;
  uint32_t blocks = 0;
  for (;;) {
    size_t got = 0;
    if (i2s_channel_read(rx, work->raw, sizeof(work->raw), &got, pdMS_TO_TICKS(100)) != ESP_OK) continue;
    for (size_t i = slot; i < got / sizeof(int32_t); i += 2) {
      work->block[filled++] = toSample(work->raw[i]);
      if (filled == led::AudioAnalyzer::kN) {
        filled = 0;
        const led::AudioFrame &f = work->analyzer.process(work->block);
        engine_->setAudio(f);
        // While a music effect runs: what it hears, every 2 s (125 blocks),
        // for checking the microphone and tuning (clap and watch the beats).
        if (++blocks % 125 == 0 && led::isMusicEffect(engine_->status().effect)) {
          int loudest = 0;
          for (int b = 1; b < led::AudioFrame::kBands; b++)
            if (f.bandDb[b] > f.bandDb[loudest]) loudest = b;
          ESP_LOGI(TAG, "Level %.0f dB (auto-gain %.0f), bass %.0f dB, loudest band %d, %lu beats", f.levelDb,
                   f.levelPeakDb, f.bassDb, loudest, (unsigned long)f.beats);
        }
      }
    }
  }
}

}  // namespace audio
}  // namespace sb
