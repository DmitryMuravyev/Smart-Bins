#pragma once

// Frame pacing shared by the LED tasks (the bin grid and the work lights).

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace sb {
namespace led {

// Waits until one `period` after the previous frame's wake time, so the time
// spent rendering is subtracted from the wait instead of being added to it
// and the frame rate stays steady.
//
// A frame can take longer than its period: a long strip needs about 30 us
// per pixel on the wire (over 16 ms from some 530 pixels on one data line),
// and a frame waits for as long as flash is being accessed (see FlashGuard).
// The schedule is then dropped and started again from the present: the task
// sleeps for one tick, so tasks of lower priority on its core always get to
// run, and the next frame follows at once. Keeping the old schedule instead
// would leave `wake` further in the past with every late frame, and once the
// frames got short again the task would run without sleeping until it had
// caught up.
inline void waitNextFrame(TickType_t &wake, TickType_t period) {
  if (static_cast<TickType_t>(xTaskGetTickCount() - wake) >= period) {
    vTaskDelay(1);
    wake = xTaskGetTickCount();
  } else {
    vTaskDelayUntil(&wake, period);
  }
}

}  // namespace led
}  // namespace sb
