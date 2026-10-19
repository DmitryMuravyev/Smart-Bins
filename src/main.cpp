// Smart Bins firmware - Task 1: boot report.
// Prints chip, flash, memory and partition information, then mounts LittleFS.

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_partition.h>

#include "FormatBytes.h"

#ifndef SB_FW_VERSION
#define SB_FW_VERSION "dev"
#endif
#ifndef SB_FLASH_MB
#define SB_FLASH_MB 0
#endif

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
  uint32_t configured = ESP.getFlashChipSize();
  ESP_LOGI(TAG, "Flash: %s detected, %s configured (build env esp32-%dmb), %lu MHz", fmt(physical),
           fmt(configured), SB_FLASH_MB, (unsigned long)(ESP.getFlashChipSpeed() / 1000000));
  if (physical != 0 && physical != configured) {
    int detectedMb = physical / (1024 * 1024);
    ESP_LOGW(TAG, "Flash size mismatch: this board has %d MB. Rebuild with env esp32-%dmb.", detectedMb,
             detectedMb);
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

static void mountFilesystem() {
  // formatOnFail = true: a blank partition is formatted on first boot.
  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    ESP_LOGE(TAG, "LittleFS mount failed");
    return;
  }
  ESP_LOGI(TAG, "LittleFS: %s used of %s, %s free", fmt(LittleFS.usedBytes()), fmt(LittleFS.totalBytes()),
           fmt(LittleFS.totalBytes() - LittleFS.usedBytes()));
}

void setup() {
  Serial.begin(115200);
  delay(500);  // give the USB-serial bridge time to open the port
  Serial.println();
  ESP_LOGI(TAG, "Smart Bins firmware %s", SB_FW_VERSION);
  printChipInfo();
  printFlashInfo();
  printMemoryInfo();
  printPartitions();
  mountFilesystem();
  ESP_LOGI(TAG, "Boot complete");
}

void loop() {
  static uint32_t last = 0;
  if (millis() - last >= 10000) {
    last = millis();
    ESP_LOGI(TAG, "Uptime %lus, free heap %s", (unsigned long)(millis() / 1000), fmt(ESP.getFreeHeap()));
  }
  delay(10);
}
