// Health: restart reason and main loop watchdog (see Health.h).

#include "Health.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>
#include <esp_timer.h>

namespace sb {
namespace health {
namespace {

const char *TAG = "health";

// loop() not seen for this long: restart.
constexpr uint32_t kLoopTimeoutMs = 60000;

// Kept across a software restart (RTC memory isn't cleared by it, but holds
// garbage after power-on, hence the magic number). Set just before the loop
// watch restarts the board, so the next boot can tell that restart from one
// asked for in the UI.
constexpr uint32_t kHangMagic = 0x48414E47;  // "HANG"
RTC_NOINIT_ATTR uint32_t hangMarker;

volatile uint32_t lastLoopMs = 0;
volatile bool paused = false;
const char *reason = "other";
bool fault = false;

void check(void *) {
  if (paused) return;
  const uint32_t idle = millis() - lastLoopMs;
  if (idle < kLoopTimeoutMs) return;
  // From the esp_timer task: logging and restarting are fine here.
  ESP_LOGE(TAG, "Main loop stuck for %lu s, restarting", (unsigned long)(idle / 1000));
  hangMarker = kHangMagic;
  esp_restart();
}

}  // namespace

void begin() {
  const bool hang = hangMarker == kHangMagic;
  hangMarker = 0;
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: reason = "power-on"; break;
    case ESP_RST_EXT: reason = "reset-pin"; break;
    case ESP_RST_SW: reason = hang ? "hang" : "restart"; fault = hang; break;
    case ESP_RST_PANIC: reason = "crash"; fault = true; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: reason = "watchdog"; fault = true; break;
    case ESP_RST_BROWNOUT: reason = "brownout"; fault = true; break;
    default: reason = "other"; break;
  }
  if (fault) {
    ESP_LOGW(TAG, "Last restart: %s", reason);
  } else {
    ESP_LOGI(TAG, "Last restart: %s", reason);
  }

  lastLoopMs = millis();
  const esp_timer_create_args_t args = {.callback = check, .arg = nullptr, .dispatch_method = ESP_TIMER_TASK,
                                        .name = "loopwatch", .skip_unhandled_events = true};
  esp_timer_handle_t timer = nullptr;
  if (esp_timer_create(&args, &timer) != ESP_OK || esp_timer_start_periodic(timer, 1000000) != ESP_OK) {
    ESP_LOGE(TAG, "Main loop watch not started");
  }
}

void loopAlive() { lastLoopMs = millis(); }

void pauseLoopWatch(bool pause) {
  lastLoopMs = millis();  // a fresh start either way
  paused = pause;
}

const char *lastRestart() { return reason; }
bool lastRestartWasFault() { return fault; }

}  // namespace health
}  // namespace sb
