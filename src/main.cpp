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
#include "ConfigStore.h"
#include "InventoryDb.h"
#include "PinMap.h"
#include "WifiLink.h"
#include "led/LedEngine.h"
#include "led/RmtStrip.h"
#include "led/WorkLights.h"
#include "web/InventoryApi.h"
#include "web/LedApi.h"
#include "web/LightsApi.h"
#include "web/LiveUpdates.h"
#include "web/Security.h"
#include "web/SystemApi.h"
#include "web/WebUi.h"

#ifndef SB_FW_VERSION
#define SB_FW_VERSION "dev"
#endif

namespace {

const char *TAG = "main";

constexpr int kButtonPin = 0;      // on-board BOOT button, active low
constexpr int kStatusLedPin = 2;   // on-board LED on most DevKitC boards
constexpr uint32_t kWifiResetHoldMs = 5000;
constexpr uint32_t kFactoryResetHoldMs = 10000;

sb::ConfigStore configStore;
sb::WifiLink network;
sb::led::LedEngine leds;
sb::led::WorkLights workLights;
sb::Security security;
sb::LiveUpdates live;
sb::inventory::InventoryDb inventoryDb;
AsyncWebServer server(80);

// Restarts requested from the web API happen here, a second later, so the
// response reaches the browser first. Written on the network task, read in loop().
volatile uint32_t restartAt = 0;
void scheduleRestart() { restartAt = millis() + 1000; }

// Set when the password changes (network task); loop() passes the new hash to
// ArduinoOTA, so the OTA library is only ever touched from the main loop.
volatile bool otaHashChanged = false;

bool mountFilesystem() {
  // formatOnFail = true: a blank partition (first boot, or after switching
  // from the old LittleFS layout) is formatted. "ffat" is the partition label
  // in partitions/*.csv; FAT runs on ESP-IDF's wear levelling layer there.
  if (!FFat.begin(true, "/ffat", 10, "ffat")) {
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
  ESP_LOGI(TAG, "Grid %ux%u bins, %u px/bin = %d px, %s %s %uV, %s from %s, %u segment(s)", g.cols, g.rows,
           g.pixelsPerBin, sb::config::gridPixels(g), sb::config::toString(g.chipset), g.colorOrder, g.voltage,
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
  digitalWrite(kStatusLedPin, on ? HIGH : LOW);
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

  mountFilesystem();
  configStore.begin();
  logConfig(configStore.get());
  openInventory(configStore.get());

  // W1-W5 in channel order; the config decides what each one drives.
  const int8_t lightPins[sb::config::kLightChannels] = {
      sb::board::defaultPin("light_w1"), sb::board::defaultPin("light_w2"), sb::board::defaultPin("light_w3"),
      sb::board::defaultPin("light_w4"), sb::board::defaultPin("light_w5")};

  // Count the addressable strips before starting any, so the RMT's transmit
  // memory is shared among exactly those (more memory per strip means more
  // tolerance for a late refill interrupt, see RmtStrip.cpp). The grid starts
  // first and so gets any spare memory.
  {
    const sb::config::AppConfig &cfg = configStore.get();
    int strips = 0;
    if (sb::board::defaultPin("grid_data_a") >= 0) strips++;
    if (cfg.grid.segments >= 2 && sb::board::defaultPin("grid_data_b") >= 0) strips++;
    for (int i = 0; i < sb::config::kLightChannels; i++) {
      if (cfg.lights[i].mode == sb::config::LightMode::Addressable && lightPins[i] >= 0) strips++;
    }
    sb::led::RmtStrip::planStrips(strips);
  }

  leds.begin(configStore.get(), sb::board::defaultPin("grid_data_a"), sb::board::defaultPin("grid_data_b"),
             sb::board::defaultPin("grid_power"));
  workLights.begin(configStore.get(), lightPins, sb::board::defaultPin("light_power"));

  // Security first: its password check is middleware, and it must be in place
  // before the server starts answering.
  security.begin(server);
  security.onChange([]() { otaHashChanged = true; });
  network.setOtaPasswordHash(security.otaHash());
  network.begin(configStore.get().deviceName, configStore.get().network, server);
  sb::registerWebUi(server, network);
  sb::registerSystemApi(server, configStore, network, security, inventoryDb, scheduleRestart);
  sb::registerInventoryApi(server, inventoryDb, configStore.get().grid.cols, configStore.get().grid.rows);
  sb::registerLedApi(server, leds, configStore);
  sb::registerLightsApi(server, workLights);
  live.begin(server, security, workLights, leds, network);
  server.begin();
  ESP_LOGI(TAG, "Boot complete, free heap %s, largest block %s", sb::formatBytes(sb::heapFree()),
           sb::formatBytes(sb::heapLargest()));
}

void loop() {
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) ESP.restart();
  if (otaHashChanged) {
    otaHashChanged = false;
    network.setOtaPasswordHash(security.otaHash());
  }
  network.loop();
  live.loop();
  handleButton();
  updateStatusLed();

  static uint32_t lastReport = 0;
  if (millis() - lastReport >= 60000) {
    lastReport = millis();
    ESP_LOGI(TAG, "Uptime %lus, free heap %s, min %s", (unsigned long)(millis() / 1000),
             sb::formatBytes(sb::heapFree()), sb::formatBytes(sb::heapMin()));
  }
  delay(2);
}
