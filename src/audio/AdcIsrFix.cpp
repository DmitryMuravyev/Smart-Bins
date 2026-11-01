// Fix for the analog microphone crashing the board ("Cache disabled but
// cached memory region accessed" on core 1) whenever flash was read or
// written while it listened.
//
// The ADC's DMA interrupt (esp_adc, adc_continuous.c) is allocated with
// ESP_INTR_FLAG_IRAM, so it keeps running while the flash cache is off, and
// its handler is in IRAM. But one function it calls,
// adc_hal_get_reading_result() (hal/adc_hal.c), is placed in IRAM only with
// CONFIG_ADC_CONTINUOUS_ISR_IRAM_SAFE, which the prebuilt Arduino libraries
// leave off. So the interrupt jumps into flash during every flash access
// and the chip panics.
//
// The libraries can't be rebuilt from here, so the build links calls to it
// to this copy instead (-Wl,--wrap=adc_hal_get_reading_result in
// platformio.ini), which lives in IRAM. It does exactly what the original
// in ESP-IDF 5.5 does (checked against its machine code): hand the DMA
// descriptors of the finished frame back to the hardware and report where
// the frame's data starts and how long it is. Its only difference is that
// it can't assert (the assert message is in flash): a missing descriptor
// is reported as "none yet" instead.

#include <esp_attr.h>
#include <hal/adc_hal.h>

extern "C" IRAM_ATTR adc_hal_dma_desc_status_t __wrap_adc_hal_get_reading_result(adc_hal_dma_ctx_t *hal,
                                                                                  const intptr_t eof_desc_addr,
                                                                                  uint8_t **buffer, uint32_t *len) {
  dma_descriptor_t *desc = hal->cur_desc_ptr;
  if (!desc || !desc->next) return ADC_HAL_DMA_DESC_NULL;
  if (reinterpret_cast<intptr_t>(desc) == eof_desc_addr) return ADC_HAL_DMA_DESC_WAITING;

  // The frame starts at the descriptor after the last one handed out and
  // ends at the one the end-of-frame interrupt named, at most eof_step
  // descriptors on. Each goes back to the DMA (owner = 1) as it is counted.
  desc = desc->next;
  desc->dw0.owner = 1;
  uint8_t *start = static_cast<uint8_t *>(desc->buffer);
  uint32_t total = desc->dw0.length;
  for (uint32_t i = 1; reinterpret_cast<intptr_t>(desc) != eof_desc_addr && i < hal->eof_step; i++) {
    desc = desc->next;
    desc->dw0.owner = 1;
    total += desc->dw0.length;
  }
  hal->cur_desc_ptr = desc;
  *buffer = start;
  *len = total;
  return ADC_HAL_DMA_DESC_VALID;
}
