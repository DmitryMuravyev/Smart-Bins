#include "audio/Microphone.h"

// Arduino.h first: it routes ESP_LOGx through Arduino's logger, which prints
// at the build's CORE_DEBUG_LEVEL. Without it these are the IDF's own macros,
// which the prebuilt core keeps at errors only, and the info lines vanish.
#include <Arduino.h>

#include <cmath>
#include <cstdint>
#include <new>

#include <driver/i2s_std.h>
#include <esp_adc/adc_continuous.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "AudioAnalyzer.h"
#include "led/LedEngine.h"

namespace sb {
namespace audio {

namespace {

const char *const TAG = "mic";

// I2S DMA: 4 buffers of 64 frames (4 ms each at 16 kHz), 2 KB in all. The
// task reads one buffer at a time; four give it 12 ms of slack before
// samples are lost, which only happens during flash writes (when the
// interrupt has to wait) and costs at most a block of analysis.
constexpr uint32_t kDmaBuffers = 4;
constexpr uint32_t kDmaFrames = 64;
constexpr size_t kI2sFrameBytes = 8;  // two 32-bit slots

// ADC DMA: frames of 64 conversions (2 bytes each on the ESP32, 3.2 ms at
// 20 kHz) into a 1 KB pool the driver keeps, the same slack as the I2S.
constexpr uint32_t kAdcFrameBytes = 64 * sizeof(adc_digi_output_data_t);
constexpr uint32_t kAdcPoolBytes = 1024;

// One read's raw bytes: a DMA buffer of either kind.
constexpr size_t kRawBytes = kDmaFrames * kI2sFrameBytes;

// The INMP441 needs 2^18 clock cycles after the clock starts before its
// output is valid (about 260 ms at 16 kHz x 64); every start skips that.
// An analog module only needs its coupling capacitor to settle a little.
constexpr uint32_t kI2sSettleMs = 300;
constexpr uint32_t kAnalogSettleMs = 100;
// How long the check listens after that.
constexpr uint32_t kListenMs = 250;

// How long the microphone keeps running after the last music effect ended:
// the effect fading out still draws from it, and a quick switch back (or
// from one music effect to another through "off") doesn't restart it.
constexpr uint32_t kLingerMs = 3000;
// How often the idle task looks whether a music effect has started.
constexpr uint32_t kIdlePollMs = 100;

// The work buffers, allocated only while the microphone runs (about 3.6 KB):
// one DMA buffer's worth of raw data, a block of mono samples for the
// analyzer, and the analyzer itself.
struct Work {
  explicit Work(uint32_t rate) : analyzer(rate) {}
  alignas(4) uint8_t raw[kRawBytes];
  float block[led::AudioAnalyzer::kN];
  led::AudioAnalyzer analyzer;
};

// A raw I2S word to a sample: the INMP441 sends 24 bits, left-aligned in
// the 32-bit slot.
inline float toSample(int32_t raw) { return static_cast<float>(raw >> 8) / 8388608.0f; }

// dB values for the log as whole numbers: printf with floating point needs
// over 1 KB of the task's stack, which the task would then have to keep.
inline int dbInt(float db) { return static_cast<int>(std::lround(db)); }

// What the check learns about one channel: the samples' range (in steps of
// the converter, 24 bits for I2S, 12 for the ADC) and loudness.
struct SlotStats {
  int32_t min = INT32_MAX, max = INT32_MIN;
  double sum = 0, sumSq = 0;
  uint32_t n = 0;

  void add(int32_t v, float fullScale) {
    if (v < min) min = v;
    if (v > max) max = v;
    const double s = v / static_cast<double>(fullScale);
    sum += s;
    sumSq += s * s;
    n++;
  }
  float mean() const { return n ? static_cast<float>(sum / n) : 0.0f; }
  // RMS without the DC offset (an analog module sits at about 1.25 V).
  float rmsDb() const {
    if (!n) return -120.0f;
    const double m = sum / n, v = sumSq / n - m * m;
    return v > 1e-12 ? 10.0f * std::log10(static_cast<float>(v)) : -120.0f;
  }
  // A microphone's output always moves a little (its own noise is a few
  // hundred steps of 24 bits), while a channel nobody drives reads one
  // value over and over. A level near full scale is not sound either but
  // a floating data line picking up the clock.
  bool liveI2s() const { return n && max - min >= 8 && rmsDb() < -10.0f; }
  // An analog module's output sits mid-range (mean as a share of the ADC's
  // range, 0..1, for the raw 0..4095 counts). A pin stuck at either end of
  // the ADC's range (shorted, or no module and a pull resistor) is not one;
  // an unconnected pin can't be told apart from a module in a silent room,
  // which is why the type is a setting and not detected.
  bool liveAnalog() const { return n && max - min >= 2 && mean() > 0.03f && mean() < 0.97f; }
};

}  // namespace

void Microphone::begin(const config::AppConfig &cfg, led::LedEngine &engine) {
  const config::PinsConfig &pins = cfg.pins;
  analog_ = cfg.microphone == config::MicType::Analog;
  sck_ = pins["mic_i2s_sck"];
  ws_ = pins["mic_i2s_ws"];
  sd_ = pins["mic_i2s_sd"];
  adcPin_ = pins["mic_analog"];
  engine_ = &engine;
  if (cfg.microphone == config::MicType::Off) {
    ESP_LOGI(TAG, "Off in the configuration; music effects off");
    return;
  }
  if (analog_ ? adcPin_ < 0 : (sck_ < 0 || ws_ < 0 || sd_ < 0)) {
    ESP_LOGI(TAG, "Not wired (no pins assigned)");
    return;
  }
  // 3 KB of stack is enough now the log lines print whole numbers (with
  // floating point it needed 4 KB).
  // Core 1 with the LED task, below it in priority: the analysis takes well
  // under a millisecond per block, and core 0 can be busy for a long time
  // with a database query in the web server, which would drop audio.
  xTaskCreatePinnedToCore(taskEntry, "mic", 3072, this, 1, nullptr, 1);
}

void Microphone::taskEntry(void *arg) {
  static_cast<Microphone *>(arg)->run();
  vTaskDelete(nullptr);
}

void *Microphone::open() {
  if (analog_) {
    adc_unit_t unit;
    adc_channel_t channel;
    esp_err_t err = adc_continuous_io_to_channel(adcPin_, &unit, &channel);
    if (err != ESP_OK || unit != ADC_UNIT_1) {
      ESP_LOGE(TAG, "GPIO%d is not an ADC1 pin", adcPin_);
      return nullptr;
    }
    adc_continuous_handle_t adc = nullptr;
    adc_continuous_handle_cfg_t handleCfg = {};
    handleCfg.max_store_buf_size = kAdcPoolBytes;
    handleCfg.conv_frame_size = kAdcFrameBytes;
    err = adc_continuous_new_handle(&handleCfg, &adc);
    if (err == ESP_OK) {
      // 12 dB attenuation: the input range is about 0.15..2.45 V, which holds
      // a MAX9814's 2 V peak-to-peak around its 1.25 V bias.
      adc_digi_pattern_config_t pattern = {};
      pattern.atten = ADC_ATTEN_DB_12;
      pattern.channel = channel;
      pattern.unit = ADC_UNIT_1;
      pattern.bit_width = ADC_BITWIDTH_12;
      adc_continuous_config_t adcCfg = {};
      adcCfg.pattern_num = 1;
      adcCfg.adc_pattern = &pattern;
      adcCfg.sample_freq_hz = kAnalogRate;
      adcCfg.conv_mode = ADC_CONV_SINGLE_UNIT_1;
      adcCfg.format = ADC_DIGI_OUTPUT_FORMAT_TYPE1;
      err = adc_continuous_config(adc, &adcCfg);
    }
    if (err == ESP_OK) err = adc_continuous_start(adc);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "ADC start failed: %s", esp_err_to_name(err));
      if (adc) adc_continuous_deinit(adc);
      return nullptr;
    }
    return adc;
  }

  i2s_chan_handle_t rx = nullptr;
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num = kDmaBuffers;
  chanCfg.dma_frame_num = kDmaFrames;
  // Both slots are read: the check finds out which one the microphone uses.
  i2s_std_config_t stdCfg = {};
  stdCfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kI2sRate);
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
    return nullptr;
  }
  return rx;
}

int Microphone::read(void *handle, uint8_t *raw, float *out, int max) {
  if (analog_) {
    const uint32_t want = static_cast<uint32_t>(max) * sizeof(adc_digi_output_data_t);
    uint32_t got = 0;
    if (adc_continuous_read(static_cast<adc_continuous_handle_t>(handle), raw, want < kRawBytes ? want : kRawBytes,
                            &got, 100) != ESP_OK)
      return 0;
    const auto *d = reinterpret_cast<const adc_digi_output_data_t *>(raw);
    const int n = got / sizeof(adc_digi_output_data_t);
    for (int i = 0; i < n; i++) out[i] = (static_cast<int>(d[i].type1.data) - 2048) / 2048.0f;
    return n;
  }
  const size_t want = static_cast<size_t>(max) * kI2sFrameBytes;
  size_t got = 0;
  if (i2s_channel_read(static_cast<i2s_chan_handle_t>(handle), raw, want < kRawBytes ? want : kRawBytes, &got,
                       pdMS_TO_TICKS(100)) != ESP_OK)
    return 0;
  const auto *words = reinterpret_cast<const int32_t *>(raw);
  const int n = got / kI2sFrameBytes;
  for (int i = 0; i < n; i++) out[i] = toSample(words[2 * i + slot_]);
  return n;
}

void Microphone::close(void *handle) {
  if (analog_) {
    auto adc = static_cast<adc_continuous_handle_t>(handle);
    adc_continuous_stop(adc);
    adc_continuous_deinit(adc);
    return;
  }
  auto rx = static_cast<i2s_chan_handle_t>(handle);
  i2s_channel_disable(rx);
  i2s_del_channel(rx);
}

void Microphone::run() {
  const uint32_t rate = sampleRate();
  const uint32_t settleSamples = rate * (analog_ ? kAnalogSettleMs : kI2sSettleMs) / 1000;

  // ---- Is anything there? ----
  // The driver and the buffers exist only for the check here and later only
  // while a music effect runs: about 6 KB of memory (driver, DMA, buffers)
  // that the web server gets back the rest of the time.
  {
    Work *work = new (std::nothrow) Work(rate);
    void *handle = work ? open() : nullptr;
    if (!handle) {
      if (!work) ESP_LOGE(TAG, "Not enough memory for the microphone");
      delete work;
      return;
    }
    SlotStats slots[2];
    const uint32_t total = settleSamples + rate * kListenMs / 1000;
    for (uint32_t seen = 0; seen < total;) {
      if (analog_) {
        const int n = read(handle, work->raw, work->block, led::AudioAnalyzer::kN);
        for (int i = 0; i < n; i++, seen++) {
          if (seen >= settleSamples) slots[0].add(std::lround(work->block[i] * 2048.0f) + 2048, 4095.0f);
        }
        if (n == 0) seen += rate / 10;  // a timeout: don't wait forever
        continue;
      }
      // I2S: both channels straight from the raw frames.
      size_t got = 0;
      if (i2s_channel_read(static_cast<i2s_chan_handle_t>(handle), work->raw, kRawBytes, &got, pdMS_TO_TICKS(100)) !=
          ESP_OK) {
        seen += rate / 10;
        continue;
      }
      const auto *words = reinterpret_cast<const int32_t *>(work->raw);
      for (size_t i = 0; i < got / kI2sFrameBytes; i++, seen++) {
        if (seen < settleSamples) continue;
        slots[0].add(words[2 * i] >> 8, 8388608.0f);
        slots[1].add(words[2 * i + 1] >> 8, 8388608.0f);
      }
    }
    close(handle);
    delete work;

    if (analog_) {
      // The level and where the signal sits in the ADC's range (about 1550
      // of 4095 for a MAX9814's 1.25 V bias), for checking the wiring.
      ESP_LOGI(TAG, "Analog GPIO%d: %d dB, samples %ld..%ld of 4095", adcPin_, dbInt(slots[0].rmsDb()),
               (long)slots[0].min, (long)slots[0].max);
      if (!slots[0].liveAnalog()) {
        ESP_LOGI(TAG, "No analog microphone on GPIO%d; music effects off", adcPin_);
        return;
      }
      ESP_LOGI(TAG, "Analog microphone on GPIO%d (room level %d dB)", adcPin_, dbInt(slots[0].rmsDb()));
    } else {
      // Both channels, for checking the wiring: the microphone's channel
      // should move a little in a quiet room (-60..-85 dB), the other one
      // should not move at all.
      for (int i = 0; i < 2; i++) {
        ESP_LOGI(TAG, "I2S %s channel: %d dB, samples %ld..%ld", i ? "right" : "left", dbInt(slots[i].rmsDb()),
                 (long)slots[i].min, (long)slots[i].max);
      }
      slot_ = slots[0].liveI2s() ? 0 : slots[1].liveI2s() ? 1 : -1;
      if (slot_ < 0) {
        ESP_LOGI(TAG, "No microphone on GPIO %d/%d/%d; music effects off", sck_, ws_, sd_);
        return;
      }
      ESP_LOGI(TAG, "Microphone found (%s channel, room level %d dB)", slot_ ? "right" : "left",
               dbInt(slots[slot_].rmsDb()));
    }
  }
  present_ = true;
  engine_->setMicrophone(true);

  for (;;) {
    // ---- Idle: wait for a music effect ----
    while (!led::isMusicEffect(engine_->status().effect)) vTaskDelay(pdMS_TO_TICKS(kIdlePollMs));

    // ---- Listen while one runs ----
    Work *work = new (std::nothrow) Work(rate);
    void *handle = work ? open() : nullptr;
    if (!handle) {
      // Memory is short right now (or the driver refused): the effect stays
      // silent, and the next try comes after a pause.
      if (!work) ESP_LOGW(TAG, "Not enough memory to start the microphone");
      delete work;
      vTaskDelay(pdMS_TO_TICKS(kLingerMs));
      continue;
    }
    ESP_LOGI(TAG, "Listening");
    const uint32_t blockMs = 1000 * led::AudioAnalyzer::kN / rate;
    uint32_t settled = 0;  // samples of the start-up time skipped so far
    uint32_t quietMs = 0;  // how long no music effect has been running
    uint32_t blocks = 0;
    int filled = 0;
    while (quietMs < kLingerMs) {
      const int n = read(handle, work->raw, work->block + filled, led::AudioAnalyzer::kN - filled);
      if (settled < settleSamples) {
        settled += n;
        continue;
      }
      filled += n;
      if (filled < led::AudioAnalyzer::kN) continue;
      filled = 0;
      const led::AudioFrame &f = work->analyzer.process(work->block);
      engine_->setAudio(f);
      const bool music = led::isMusicEffect(engine_->status().effect);
      quietMs = music ? 0 : quietMs + blockMs;
      // While a music effect runs: what it hears, about every 2 s, for
      // checking the microphone and tuning (clap and watch the beats).
      if (++blocks % (2000 / blockMs) == 0 && music) {
        int loudest = 0;
        for (int b = 1; b < led::AudioFrame::kBands; b++)
          if (f.bandDb[b] > f.bandDb[loudest]) loudest = b;
        ESP_LOGI(TAG, "Level %d dB (auto-gain %d), bass %d dB, loudest band %d, %lu beats", dbInt(f.levelDb),
                 dbInt(f.levelPeakDb), dbInt(f.bassDb), loudest, (unsigned long)f.beats);
      }
    }
    close(handle);
    delete work;
    engine_->setAudio(led::AudioFrame());  // silence until the next start
    ESP_LOGI(TAG, "Stopped (no music effect running)");
  }
}

}  // namespace audio
}  // namespace sb
