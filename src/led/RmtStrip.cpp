#include "RmtStrip.h"

#include <esp_attr.h>
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

// Flash-write gate (see FlashGuard in the header). write() holds it while
// starting a frame; a FlashGuard holds it for its whole life, so no frame can
// start during a flash write. The strip list lets the guard wait for frames
// that were already on the wire when it took the gate.
constexpr int kMaxStrips = 8;  // the ESP32 has 8 RMT channels
RmtStrip *strips[kMaxStrips] = {};

SemaphoreHandle_t gate() {
  // Function-local static: created on first use, thread-safe in C++11.
  static SemaphoreHandle_t g = xSemaphoreCreateMutex();
  return g;
}

// Longest wait for a frame in flight: 1000 RGBW pixels take about 40 ms.
constexpr uint32_t kFrameWaitMs = 60;

// Transmit memory. The ESP32's RMT has 8 blocks of 64 symbols (one symbol is
// one data bit). A frame is longer than that, so the driver sends from the
// block as a ring: when half of it has gone out, an interrupt refills that
// half while the other half is on the wire. If the interrupt comes late (other
// interrupts, or a critical section elsewhere masking it), the hardware runs
// into stale symbols: the frame is cut off or repeats bits, which the strip
// shows as a flicker or a pixel lit past the end of the grid. Half a block is
// all the slack there is: 32 bits = 40 us with one block, 80 us with two,
// and so on, so each strip is given as many blocks as the plan allows.
constexpr int kMemBlocks = 8;
constexpr int kSymbolsPerBlock = 64;
int plannedStrips = 0;         // strips still to be started (planStrips)
int freeBlocks = kMemBlocks;   // blocks not yet given to a strip

// Interrupt level for the refill interrupt. 3 is the highest a handler written
// in C may use; it lets the refill pre-empt the many level-1 interrupts
// (timers, GPIO, UART, the scheduler tick). All channels share one interrupt,
// so every strip must use the same level.
constexpr int kIntrPriority = 3;
}  // namespace

void RmtStrip::planStrips(int count) {
  plannedStrips = count;
  freeBlocks = kMemBlocks;
}

// Done interrupt for one frame. Only clears the flag; returns false because
// no higher-priority task was woken. In IRAM in case the build makes the RMT
// interrupt flash-safe, where a handler in flash would crash during a write.
IRAM_ATTR bool RmtStrip::onDone(rmt_channel_handle_t, const rmt_tx_done_event_data_t *, void *ctx) {
  static_cast<RmtStrip *>(ctx)->inFlight_ = false;
  return false;
}

FlashGuard::FlashGuard() { lock(); }

FlashGuard::~FlashGuard() { unlock(); }

void FlashGuard::lock() {
  xSemaphoreTake(gate(), portMAX_DELAY);
  // Polling rather than rmt_tx_wait_all_done(): that call takes finished
  // frames off the driver's internal queue, so running it here as well as in
  // the strip's own task (waitDone) would let one of them miss the event.
  const TickType_t start = xTaskGetTickCount();
  for (RmtStrip *s : strips) {
    while (s && s->inFlight_ && xTaskGetTickCount() - start < pdMS_TO_TICKS(kFrameWaitMs)) vTaskDelay(1);
  }
}

void FlashGuard::unlock() { xSemaphoreGive(gate()); }

bool RmtStrip::begin(int gpio, const BitTiming &timing, bool invert) {
  end();

  rmt_tx_channel_config_t ch = {};
  ch.gpio_num = static_cast<gpio_num_t>(gpio);
  ch.clk_src = RMT_CLK_SRC_DEFAULT;
  ch.resolution_hz = kResolutionHz;
  // This strip's share of the transmit memory: the free blocks divided among
  // the strips still to come, rounded up. The grid lines are started first, so
  // they get any spare block: with 5 strips that is 2+2+2+1+1, with 3 it is
  // 3+3+2, a single strip gets all 8.
  // A channel with N blocks also uses the blocks of the next N-1 channels,
  // which then can't be used; the driver handles that as long as the blocks
  // are free, which they are when strips are started in turn like this.
  int blocks = 1;
  if (plannedStrips > 0) {
    blocks = (freeBlocks + plannedStrips - 1) / plannedStrips;
    if (blocks < 1) blocks = 1;
  }
  // Only one frame is ever queued (write() waits for the previous one), but a
  // depth of 2 leaves room so rmt_transmit() never blocks.
  ch.trans_queue_depth = 2;
  ch.intr_priority = kIntrPriority;
  ch.flags.invert_out = invert;
  ch.mem_block_symbols = blocks * kSymbolsPerBlock;
  esp_err_t err = rmt_new_tx_channel(&ch, &channel_);
  if (err != ESP_OK && blocks > 1) {
    // Not enough contiguous free blocks (more strips than planned): fall back
    // to the minimum rather than leave the strip dark.
    blocks = 1;
    ch.mem_block_symbols = kSymbolsPerBlock;
    err = rmt_new_tx_channel(&ch, &channel_);
  }
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
  rmt_tx_event_callbacks_t callbacks = {};
  callbacks.on_trans_done = onDone;
  if (err == ESP_OK) err = rmt_tx_register_event_callbacks(channel_, &callbacks, this);
  if (err == ESP_OK) err = rmt_enable(channel_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "GPIO%d: RMT setup failed (%s)", gpio, esp_err_to_name(err));
    end();
    return false;
  }
  xSemaphoreTake(gate(), portMAX_DELAY);
  for (RmtStrip *&slot : strips) {
    if (!slot) {
      slot = this;
      break;
    }
  }
  xSemaphoreGive(gate());
  if (plannedStrips > 0) {
    plannedStrips--;
    freeBlocks = freeBlocks > blocks ? freeBlocks - blocks : 0;
  }
  ESP_LOGI(TAG, "GPIO%d ready (%s, %d symbols)", gpio, invert ? "inverted" : "normal", blocks * kSymbolsPerBlock);
  return true;
}

void RmtStrip::end() {
  xSemaphoreTake(gate(), portMAX_DELAY);
  for (RmtStrip *&slot : strips) {
    if (slot == this) slot = nullptr;
  }
  xSemaphoreGive(gate());
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
  // Waits here while a flash write is in progress (FlashGuard holds the gate).
  xSemaphoreTake(gate(), portMAX_DELAY);
  inFlight_ = true;
  const bool ok = rmt_transmit(channel_, encoder_, data, len, &tx) == ESP_OK;
  if (!ok) inFlight_ = false;
  xSemaphoreGive(gate());
  busy_ = ok;
  return ok;
}

}  // namespace led
}  // namespace sb
