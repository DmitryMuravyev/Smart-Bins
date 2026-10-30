#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace sb {

// Holds a FreeRTOS mutex for the lifetime of the object (RAII), so it is
// released on every return path, including early returns.
class MutexLock {
 public:
  explicit MutexLock(SemaphoreHandle_t m) : m_(m) { xSemaphoreTake(m_, portMAX_DELAY); }
  ~MutexLock() { xSemaphoreGive(m_); }
  MutexLock(const MutexLock &) = delete;
  MutexLock &operator=(const MutexLock &) = delete;

 private:
  SemaphoreHandle_t m_;
};

}  // namespace sb
