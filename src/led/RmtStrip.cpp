#include "RmtStrip.h"

#include <esp_log.h>

namespace sb {
namespace led {

namespace {
const char *TAG = "rmt";
// RMT tick rate. 40 MHz (25 ns per tick) divides the 80 MHz APB clock evenly
// and is fine enough that every LED timing is within 12 ns of its target.
constexpr uint32_t kResolutionHz = 40000000;

// Nanoseconds to ticks, rounded to the nearest tick (+12 is half of 25 ns).
constexpr uint16_t ticks(uint16_t ns) { return static_cast<uint16_t>((ns + 12) / 25); }
}  // namespace

bool RmtStrip::begin(int gpio, const BitTiming &timing, bool invert) {
  end();

  rmt_tx_channel_config_t ch = {};
  ch.gpio_num = static_cast<gpio_num_t>(gpio);
  ch.clk_src = RMT_CLK_SRC_DEFAULT;
  ch.resolution_hz = kResolutionHz;
  // One 64-symbol RAM block per channel: the ESP32 has eight, so the two grid
  // lines and three work strips all get their own. The driver refills the
  // block from an interrupt while it transmits, so frames of any length work.
  ch.mem_block_symbols = 64;
  // Only one frame is ever queued (write() waits for the previous one), but a
  // depth of 2 leaves room so rmt_transmit() never blocks.
  ch.trans_queue_depth = 2;
  ch.flags.invert_out = invert;
  esp_err_t err = rmt_new_tx_channel(&ch, &channel_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "GPIO%d: no RMT channel (%s)", gpio, esp_err_to_name(err));
    channel_ = nullptr;
    return false;
  }

  // The bytes encoder turns every data bit into one RMT symbol: a high period
  // then a low period, whose lengths say whether the bit is 0 or 1. Bits go out
  // most significant first, which is what every WS281x/SK6812 chip expects.
  rmt_bytes_encoder_config_t enc = {};
  enc.bit0.level0 = 1;
  enc.bit0.duration0 = ticks(timing.t0h);
  enc.bit0.level1 = 0;
  enc.bit0.duration1 = ticks(timing.t0l);
  enc.bit1.level0 = 1;
  enc.bit1.duration0 = ticks(timing.t1h);
  enc.bit1.level1 = 0;
  enc.bit1.duration1 = ticks(timing.t1l);
  enc.flags.msb_first = 1;
  err = rmt_new_bytes_encoder(&enc, &encoder_);
  if (err == ESP_OK) err = rmt_enable(channel_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "GPIO%d: RMT setup failed (%s)", gpio, esp_err_to_name(err));
    end();
    return false;
  }
  ESP_LOGI(TAG, "GPIO%d ready (%s)", gpio, invert ? "inverted" : "normal");
  return true;
}

void RmtStrip::end() {
  if (channel_) {
    if (busy_) rmt_tx_wait_all_done(channel_, 100);
    rmt_disable(channel_);
    rmt_del_channel(channel_);
    channel_ = nullptr;
  }
  if (encoder_) {
    rmt_del_encoder(encoder_);
    encoder_ = nullptr;
  }
  busy_ = false;
}

bool RmtStrip::waitDone(uint32_t timeoutMs) {
  if (!channel_ || !busy_) return true;
  if (rmt_tx_wait_all_done(channel_, static_cast<int>(timeoutMs)) != ESP_OK) return false;
  busy_ = false;
  return true;
}

bool RmtStrip::write(const uint8_t *data, size_t len) {
  if (!channel_ || len == 0) return false;
  if (!waitDone(50)) return false;
  rmt_transmit_config_t tx = {};
  tx.loop_count = 0;
  tx.flags.eot_level = 0;  // line rests low between frames: the strip latches after ~300 us
  if (rmt_transmit(channel_, encoder_, data, len, &tx) != ESP_OK) return false;
  busy_ = true;
  return true;
}

}  // namespace led
}  // namespace sb
