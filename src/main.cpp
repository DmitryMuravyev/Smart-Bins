// Smart Bins firmware entry point.
//
// Boot order: hold LED outputs low, print the boot report, mount the FAT partition,
// load the configuration, start the LED engine and work lights, then bring up
// Wi-Fi (or the setup portal), mDNS, over-the-air updates and the web server.

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <FFat.h>

#include <ctime>

#include "BootReport.h"
#include "Health.h"
#include "ConfigStore.h"
#include "InventoryDb.h"
#include "MemoryProfile.h"
#include "PinMap.h"
#include "WifiLink.h"
#include "audio/Microphone.h"
#include "led/LedEngine.h"
#include "led/RmtStrip.h"
#include "led/WorkLights.h"
#include "web/InventoryApi.h"
#include "web/BackupApi.h"
#include "web/PhotoApi.h"
#include "MotionSensor.h"
#include "PowerSupply.h"
#include "PhotoStore.h"
#include "web/LedApi.h"
#include "web/LightsApi.h"
#include "web/LiveUpdates.h"
#include "web/Security.h"
#include "web/SendGate.h"
#include "web/SystemApi.h"
#include "web/WebUi.h"

#ifndef SB_FW_VERSION
#define SB_FW_VERSION "dev"
#endif

// The loop task's stack (8 KB by default): smaller without PSRAM, see
// kLoopStack in MemoryProfile.h.
SET_LOOP_TASK_STACK_SIZE(sb::mem::kLoopStack);

namespace {

const char *TAG = "main";

constexpr int kButtonPin = 0;      // on-board BOOT button, active low
int statusLedPin = -1;             // from the pin map (status_led); -1 = none
constexpr uint32_t kWifiResetHoldMs = 5000;
constexpr uint32_t kFactoryResetHoldMs = 10000;

sb::ConfigStore configStore;
sb::WifiLink network;
sb::led::LedEngine leds;
sb::audio::Microphone microphone;
sb::led::WorkLights workLights;
sb::Security security;
sb::LiveUpdates live;
sb::inventory::InventoryDb inventoryDb;
sb::PhotoStore photos;
sb::MotionSensor motion;
sb::PowerSupply supply;  // PS-ON / PG of the LED supply, when wired
AsyncWebServer server(80);

// Restarts requested from the web API happen here, a second later, so the
// response reaches the browser first. Written on the network task, read in loop().
volatile uint32_t restartAt = 0;
void scheduleRestart() { restartAt = millis() + 1000; }

// Longest wait for the lights to go dark before a restart: their shutdown
// fade (at most 1s, kShutdownFadeMs) plus a margin. Past it the restart goes
// ahead anyway.
constexpr uint32_t kRestartDarkMs = 3000;

// Before every restart from here: fades the grid and the work lights to dark
// and switches their strips off (see LedEngine::shutdown()), so a restart
// doesn't leave the last picture latched in the pixels or light the end of a
// strip with a frame cut short. Runs on the loop task, which the restart
// ends anyway; the web server keeps answering meanwhile.
void darkenForRestart() {
  leds.shutdown();
  workLights.shutdown();
  const uint32_t start = millis();
  while (!(leds.shutDown() && workLights.shutDown()) && millis() - start < kRestartDarkMs) {
    sb::health::loopAlive();
    delay(10);
  }
}

// Set when the password changes (network task); loop() passes the new hash to
// ArduinoOTA, so the OTA library is only ever touched from the main loop.
volatile bool otaHashChanged = false;

// Logs how much heap the last setup step took. A board without PSRAM has
// little to spare, so the boot log shows where it goes (Wi-Fi, LED buffers,
// the database, the microSD card...) instead of only the total at the end.
// At info level, so a log sent in from the field has the numbers.
void logHeapStep(const char *step) {
  static uint32_t last = 0;
  const uint32_t now = sb::heapFree();
  if (last) {
    ESP_LOGI(TAG, "Heap after %s: %s free (%s used by it)", step, sb::formatBytes(now),
             sb::formatBytes(last > now ? last - now : 0));
  } else {
    ESP_LOGI(TAG, "Heap before %s: %s free", step, sb::formatBytes(now));
  }
  last = now;
}

bool mountFilesystem() {
  // formatOnFail = true: a blank partition (first boot, or one that holds
  // another file system) is formatted. "ffat" is the partition label
  // in partitions/*.csv; FAT runs on ESP-IDF's wear levelling layer there.
  // 4 files open at once without PSRAM: the database, its journal,
  // config.json and one spare. Each slot can cost a sector-sized (4 KB)
  // buffer of heap, so the default of 10 would waste around 24 KB. With PSRAM 8,
  // so photo sends and a backup can run beside the database.
  if (!FFat.begin(true, "/ffat", sb::mem::kMaxOpenFiles, "ffat")) {
    ESP_LOGE(TAG, "FAT partition mount failed");
    return false;
  }
  ESP_LOGI(TAG, "Storage: %s used of %s", sb::formatBytes(FFat.usedBytes()), sb::formatBytes(FFat.totalBytes()));
  return true;
}

// The inventory database: /inventory.db on the FAT partition. A failure is
// logged and the board runs on without it (the inventory API answers 503).
void openInventory(const sb::config::AppConfig &cfg) {
  // Writes pause the LED strips (flash writes stall their refill interrupt).
  inventoryDb.setWriteHooks({[] { sb::led::FlashGuard::lock(); }, [] { sb::led::FlashGuard::unlock(); }});
  // Reads too: on the ESP32 reading the file switches the flash cache off
  // like a write does, and a search reads a lot of it (see FlashGuard).
  inventoryDb.setReadHooks({[] { sb::led::FlashGuard::lock(); }, [] { sb::led::FlashGuard::unlock(); }});
  // Record times only once network time has arrived: before that the clock
  // counts from 1970 and the times would be meaningless.
  inventoryDb.setClock([]() -> int64_t {
    const time_t now = time(nullptr);
    return now > 1700000000 ? static_cast<int64_t>(now) : 0;
  });
  std::string err;
  const uint32_t heapBefore = sb::heapFree();
  if (!inventoryDb.open("/ffat/inventory.db", err)) {
    ESP_LOGE(TAG, "Inventory database unavailable: %s", err.c_str());
    return;
  }
  if (inventoryDb.syncBins(cfg.grid.cols, cfg.grid.rows, err) != sb::inventory::Result::Ok) {
    ESP_LOGE(TAG, "Bin table update failed: %s", err.c_str());
  }
  sb::inventory::Stats s;
  if (inventoryDb.stats(s, err) == sb::inventory::Result::Ok) {
    ESP_LOGI(TAG, "Inventory: %lld parts in %lld of %lld bins, database %s, %s of RAM", (long long)s.parts,
             (long long)s.binsUsed, (long long)s.binsTotal, sb::formatBytes(s.dbBytes),
             sb::formatBytes(heapBefore > sb::heapFree() ? heapBefore - sb::heapFree() : 0));
  }
}

void logConfig(const sb::config::AppConfig &cfg) {
  const auto &g = cfg.grid;
  ESP_LOGI(TAG, "Device \"%s\", language %s", cfg.deviceName, cfg.language);
  ESP_LOGI(TAG, "Grid %ux%u bins, %u px/bin = %d px, %s %s, %s from %s, %u segment(s)", g.cols, g.rows,
           g.pixelsPerBin, sb::config::gridPixels(g), sb::config::toString(g.chipset), g.colorOrder,
           sb::config::toString(g.layout), sb::config::toString(g.start), g.segments);
  for (int i = 0; i < sb::config::kLightChannels; i++) {
    const auto &l = cfg.lights[i];
    if (l.mode == sb::config::LightMode::Addressable) {
      ESP_LOGI(TAG, "Light W%d: addressable, %u px %s %s%s", i + 1, l.pixels, sb::config::toString(l.chipset),
               l.colorOrder, l.invertData ? ", inverted data" : "");
    } else {
      ESP_LOGI(TAG, "Light W%d: %s", i + 1, sb::config::toString(l.mode));
    }
  }
  // The pin map was printed above as the defaults; these are the changes.
  for (size_t i = 0; i < sb::board::kDefaultPinCount; i++) {
    const auto &d = sb::board::kDefaultPins[i];
    if (cfg.pins.gpio[i] != d.gpio) ESP_LOGI(TAG, "Pin %s: GPIO%d (default %d)", d.name, cfg.pins.gpio[i], d.gpio);
  }
}

// BOOT button: hold 5 s to forget Wi-Fi (and a fixed IP address), 10 s for a
// factory reset (Wi-Fi, config and web password).
// Acts on release, so holding through 5 s on the way to 10 s doesn't trigger the first.
void handleButton() {
  static uint32_t pressedAt = 0;
  const bool down = digitalRead(kButtonPin) == LOW;
  const uint32_t now = millis();
  if (down && pressedAt == 0) {
    pressedAt = now;
  } else if (!down && pressedAt != 0) {
    const uint32_t held = now - pressedAt;
    pressedAt = 0;
    if (held >= kFactoryResetHoldMs) {
      ESP_LOGW(TAG, "Factory reset");
      configStore.erase();
      security.erase();
      network.forgetCredentials();
      darkenForRestart();
      delay(200);
      ESP.restart();
    } else if (held >= kWifiResetHoldMs) {
      network.forgetCredentials();
      // A fixed address that doesn't fit the next network would leave the
      // board unreachable there, so a Wi-Fi reset goes back to DHCP too. The
      // restart applies it; the setup portal opens again after it.
      if (configStore.get().network.staticIp) {
        sb::config::AppConfig cfg = configStore.get();
        cfg.network.staticIp = false;
        std::string err;
        configStore.save(cfg, err);
        darkenForRestart();
        delay(200);
        ESP.restart();
      }
    }
  }
}

// Status LED: solid while the button is held past a reset threshold, fast blink
// while the setup portal is open, slow blink while connecting, off when connected.
void updateStatusLed() {
  static uint32_t pressedSince = 0;
  const uint32_t now = millis();
  const bool down = digitalRead(kButtonPin) == LOW;
  if (!down) pressedSince = 0;
  else if (pressedSince == 0) pressedSince = now;

  bool on;
  if (down && now - pressedSince >= kFactoryResetHoldMs) {
    on = (now / 60) % 2;  // flicker: release now for a factory reset
  } else if (down && now - pressedSince >= kWifiResetHoldMs) {
    on = true;            // solid: release now to forget Wi-Fi
  } else {
    switch (network.state()) {
      case sb::WifiLink::State::Connected: on = network.portalActive() && (now / 250) % 2; break;
      case sb::WifiLink::State::Connecting: on = (now % 1000) < 100; break;
      default: on = (now / 250) % 2; break;
    }
  }
  if (statusLedPin >= 0) digitalWrite(statusLedPin, on ? HIGH : LOW);
}

}  // namespace

void setup() {
  sb::holdOutputsLow();
  pinMode(kButtonPin, INPUT_PULLUP);
  Serial.begin(115200);
  delay(500);  // give the USB-serial bridge time to open the port
  Serial.println();
  ESP_LOGI(TAG, "Smart Bins firmware %s", SB_FW_VERSION);
  sb::printBootReport();
  sb::health::begin();  // why the board restarted; starts the main loop watch

  logHeapStep("storage");
  mountFilesystem();
  configStore.begin();
  logConfig(configStore.get());
  // The pin map from the configuration (the defaults until changed on the
  // Hardware page). The early hold above used the default map.
  const sb::config::PinsConfig &pins = configStore.get().pins;
  sb::holdOutputsLow(pins.gpio);
  statusLedPin = pins["status_led"];
  if (statusLedPin >= 0) pinMode(statusLedPin, OUTPUT);
  logHeapStep("storage and config");
  openInventory(configStore.get());
  logHeapStep("database");
  // Photos go on a microSD card when one is in the slot, else in flash.
  photos.begin(pins["sd_sck"], pins["sd_miso"], pins["sd_mosi"], pins["sd_cs"]);
  logHeapStep("photo storage");

  // W1-W5 in channel order; the config decides what each one drives.
  const int8_t lightPins[sb::config::kLightChannels] = {
      pins["light_w1"], pins["light_w2"], pins["light_w3"], pins["light_w4"], pins["light_w5"]};

  // Count the addressable strips before starting any, so the RMT's transmit
  // memory is shared among exactly those (more memory per strip means more
  // tolerance for a late refill interrupt, see RmtStrip.cpp). The grid starts
  // first and so gets any spare memory.
  {
    const sb::config::AppConfig &cfg = configStore.get();
    int strips = 0;
    if (pins["grid_data_a"] >= 0) strips++;
    if (cfg.grid.segments >= 2 && pins["grid_data_b"] >= 0) strips++;
    for (int i = 0; i < sb::config::kLightChannels; i++) {
      if (cfg.lights[i].mode == sb::config::LightMode::Addressable && lightPins[i] >= 0) strips++;
    }
    sb::led::RmtStrip::planStrips(strips);
  }

  // The LED supply's PS-ON first: the grid and the work lights ask it for
  // power before they switch their own rails on.
  supply.begin(pins["ps_on"], pins["power_good"]);
  leds.setSupply(&supply);
  workLights.setSupply(&supply);
  leds.begin(configStore.get(), pins["grid_data_a"], pins["grid_data_b"], pins["grid_power"]);
  workLights.begin(configStore.get(), lightPins, pins["light_power"]);
  logHeapStep("LED strips and work lights");
  // Optional: a motion sensor on the analog microphone's pin, a master
  // switch over the lights (see MotionSensor.h).
  motion.begin(configStore.get(), workLights, leds);
  sb::setMotionSensor(&motion);
  // Optional: checks in the background whether a microphone answers, and
  // feeds the music effects if one does.
  microphone.begin(configStore.get(), leds);

  // Security first: its password check is middleware, and it must be in place
  // before the server starts answering.
  security.begin(server);
  security.onChange([]() { otaHashChanged = true; });
  network.setOtaPasswordHash(security.otaHash());
  network.begin(configStore.get().deviceName, configStore.get().network, server);
  logHeapStep("Wi-Fi");
  sb::registerWebUi(server, network);
  sb::registerSystemApi(server, configStore, network, security, inventoryDb, photos, scheduleRestart);
  sb::setPowerSupply(&supply);
  sb::registerInventoryApi(server, inventoryDb, configStore.get().grid.cols, configStore.get().grid.rows,
                           [](const std::string &photo) { photos.remove(photo); });
  sb::registerPhotoApi(server, photos, inventoryDb, security);
  sb::registerBackupApi(server, inventoryDb, photos, configStore, security, scheduleRestart,
                        configStore.get().grid.cols, configStore.get().grid.rows);
  sb::registerLedApi(server, leds, configStore);
  sb::registerLightsApi(server, workLights);
  live.begin(server, security, workLights, leds, network);
  server.begin();
  logHeapStep("web server");
  ESP_LOGI(TAG, "Boot complete, free heap %s, largest block %s", sb::formatBytes(sb::heapFree()),
           sb::formatBytes(sb::heapLargest()));
}

// Memory diagnostics. The 60-second report shows the lowest free heap since
// boot, but not what drove it there. This logs every new low (in steps of
// 1 KB, so a slow slide doesn't flood the log) together with what the web
// server was doing at that moment: file sends running and waiting (see
// SendGate) and open WebSocket connections. The largest free block shows
// fragmentation: a big request can fail while the total still looks fine.
// The low is noticed up to a loop pass late, so the activity is close to,
// not exactly, what caused it.
void logHeapLow() {
  static uint32_t logged = UINT32_MAX;
  const uint32_t low = sb::heapMin();
  if (logged != UINT32_MAX && low + 1024 > logged) return;
  logged = low;
  // Every new low at debug level (for tuning); a warning only once it gets
  // tight, which is the line worth seeing in the field.
  constexpr uint32_t kWarnBelow = 24 * 1024;
  char line[160];
  snprintf(line, sizeof(line), "Heap low %s (free %s, largest block %s): %d file sends, %u waiting, %u WebSocket clients",
           sb::formatBytes(low), sb::formatBytes(sb::heapFree()), sb::formatBytes(sb::heapLargest()),
           sb::sendGate().active(), (unsigned)sb::sendGate().waiting(), (unsigned)live.clients());
  if (low < kWarnBelow) {
    ESP_LOGW(TAG, "%s", line);
  } else {
    ESP_LOGD(TAG, "%s", line);
  }
}

// How much of each long-running task's stack has never been used. Stacks are
// fixed blocks taken from the same heap, so one with lots to spare can be
// made smaller to free RAM; one near zero is about to overflow.
void logStacks() {
  static const char *const kTasks[] = {"loopTask", "async_tcp", "leds", "lights", "mic"};
  String line;
  for (const char *name : kTasks) {
    TaskHandle_t task = xTaskGetHandle(name);
    if (!task) continue;
    if (line.length()) line += ", ";
    line += name;
    line += ' ';
    line += sb::formatBytes(uxTaskGetStackHighWaterMark(task));
  }
  ESP_LOGD(TAG, "Unused stack: %s", line.c_str());
}

void loop() {
  sb::health::loopAlive();
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) {
    configStore.flush();  // a change a page made just before (see ConfigStore::update())
    darkenForRestart();
    ESP.restart();
  }
  configStore.loop();
  if (otaHashChanged) {
    otaHashChanged = false;
    network.setOtaPasswordHash(security.otaHash());
  }
  network.loop();
  motion.loop();
  live.loop();
  sb::sendGate().loop();  // starts file sends that waited for a free slot
  handleButton();
  updateStatusLed();

  logHeapLow();

  static uint32_t lastReport = 0;
  if (millis() - lastReport >= 60000) {
    lastReport = millis();
    if (sb::mem::kPsram) {
      ESP_LOGD(TAG, "Uptime %lus, free heap %s, min %s, PSRAM free %s", (unsigned long)(millis() / 1000),
               sb::formatBytes(sb::heapFree()), sb::formatBytes(sb::heapMin()), sb::formatBytes(sb::psramFree()));
    } else {
      ESP_LOGD(TAG, "Uptime %lus, free heap %s, min %s", (unsigned long)(millis() / 1000),
               sb::formatBytes(sb::heapFree()), sb::formatBytes(sb::heapMin()));
    }
    logStacks();
  }
  delay(2);
}
