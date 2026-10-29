// Boot report: chip, flash, memory, partition and pin information.

#include "BootReport.h"

#include <Arduino.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_partition.h>

#include "FormatBytes.h"
#include "PinMap.h"

#ifndef SB_FLASH_MB
#define SB_FLASH_MB 0
#endif

namespace sb {

static const char *TAG = "boot";

static const char *fmt(uint64_t bytes) {
  // Rotating buffers so several values can be used in one log line.
  static char bufs[4][24];
  static uint8_t next = 0;
  char *buf = bufs[next];
  next = (next + 1) % 4;
  return sb::formatBytes(bytes, buf, sizeof(bufs[0]));
}

static void printChipInfo() {
  esp_chip_info_t info;
  esp_chip_info(&info);
  ESP_LOGI(TAG, "Chip: %s rev %d.%d, %d core(s), %lu MHz", ESP.getChipModel(), info.revision / 100,
           info.revision % 100, info.cores, (unsigned long)ESP.getCpuFreqMHz());
  ESP_LOGI(TAG, "Features: WiFi%s%s%s", (info.features & CHIP_FEATURE_BT) ? ", BT" : "",
           (info.features & CHIP_FEATURE_BLE) ? ", BLE" : "",
           (info.features & CHIP_FEATURE_EMB_PSRAM) ? ", embedded PSRAM" : "");
  ESP_LOGI(TAG, "SDK: %s, Arduino core %d.%d.%d", ESP.getSdkVersion(), ESP_ARDUINO_VERSION_MAJOR,
           ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
}

static void printFlashInfo() {
  uint32_t physical = 0;
  esp_flash_get_physical_size(esp_flash_default_chip, &physical);
  // Compare against the build environment: ESP.getFlashChipSize() can report the
  // physical size even when a smaller partition layout was flashed.
  const uint32_t built = (uint32_t)SB_FLASH_MB * 1024 * 1024;
  ESP_LOGI(TAG, "Flash: %s detected, firmware built for %s (env esp32-%dmb), %lu MHz", fmt(physical), fmt(built),
           SB_FLASH_MB, (unsigned long)(ESP.getFlashChipSpeed() / 1000000));
  if (physical == 0 || built == 0 || physical == built) {
    return;
  }
  const int detectedMb = physical / (1024 * 1024);
  if (physical > built) {
    ESP_LOGW(TAG, "Only %d of %d MB flash in use. Flash env esp32-%dmb to use all of it.", SB_FLASH_MB, detectedMb,
             detectedMb);
  } else {
    ESP_LOGE(TAG, "Flash too small: board has %d MB, firmware expects %d MB. Flash env esp32-%dmb.", detectedMb,
             SB_FLASH_MB, detectedMb);
  }
}

static void printMemoryInfo() {
  ESP_LOGI(TAG, "Heap: %s free of %s, largest block %s, min ever %s", fmt(ESP.getFreeHeap()),
           fmt(ESP.getHeapSize()), fmt(ESP.getMaxAllocHeap()), fmt(ESP.getMinFreeHeap()));
  if (ESP.getPsramSize() > 0) {
    ESP_LOGI(TAG, "PSRAM: %s free of %s", fmt(ESP.getFreePsram()), fmt(ESP.getPsramSize()));
  } else {
    ESP_LOGI(TAG, "PSRAM: none");
  }
  ESP_LOGI(TAG, "Sketch: %s used, %s free in app slot", fmt(ESP.getSketchSize()), fmt(ESP.getFreeSketchSpace()));
}

static void printPartitions() {
  ESP_LOGI(TAG, "Partitions:");
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  for (; it != nullptr; it = esp_partition_next(it)) {
    const esp_partition_t *p = esp_partition_get(it);
    ESP_LOGI(TAG, "  %-9s type %02x/%02x  at 0x%06lx  %s", p->label, p->type, p->subtype, (unsigned long)p->address,
             fmt(p->size));
  }
  esp_partition_iterator_release(it);
}

// Drive every LED-related output LOW before anything else, so power switches stay
// off and no strip sees random data while the firmware starts.
void holdOutputsLow() {
  for (const auto &pin : sb::board::kDefaultPins) {
    if (pin.lowAtBoot && pin.gpio != sb::board::kNoPin) {
      pinMode(pin.gpio, OUTPUT);
      digitalWrite(pin.gpio, LOW);
    }
  }
}

static void printPinMap() {
  ESP_LOGI(TAG, "Pin map (defaults):");
  for (const auto &pin : sb::board::kDefaultPins) {
    if (pin.gpio == sb::board::kNoPin) {
      ESP_LOGI(TAG, "  %-12s -", pin.name);
    } else {
      ESP_LOGI(TAG, "  %-12s GPIO%d", pin.name, pin.gpio);
    }
  }
}

void printBootReport() {
  printChipInfo();
  printFlashInfo();
  printMemoryInfo();
  printPartitions();
  printPinMap();
}

const char *formatBytes(uint64_t bytes) { return fmt(bytes); }

}  // namespace sb
