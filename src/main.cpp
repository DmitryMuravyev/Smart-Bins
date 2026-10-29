// Smart Bins firmware entry point.
//
// Boot order: hold LED outputs low, print the boot report, mount LittleFS,
// load the configuration, then bring up Wi-Fi (or the setup portal), mDNS,
// over-the-air updates and the web server.

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include "BootReport.h"
#include "ConfigStore.h"
#include "WifiLink.h"

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
AsyncWebServer server(80);

bool mountFilesystem() {
  // formatOnFail = true: a blank partition is formatted on first boot.
  // The partition is labelled "spiffs" (the Arduino default) even though it holds
  // LittleFS: a label of "littlefs" collides with the IDF subtype name and makes
  // the partition tool warn.
  if (!LittleFS.begin(true, "/littlefs", 10, "spiffs")) {
    ESP_LOGE(TAG, "LittleFS mount failed");
    return false;
  }
  ESP_LOGI(TAG, "LittleFS: %s used of %s", sb::formatBytes(LittleFS.usedBytes()),
           sb::formatBytes(LittleFS.totalBytes()));
  return true;
}

void logConfig(const sb::config::AppConfig &cfg) {
  const auto &g = cfg.grid;
  ESP_LOGI(TAG, "Device \"%s\", language %s", cfg.deviceName, cfg.language);
  ESP_LOGI(TAG, "Grid %ux%u bins, %u px/bin = %d px, %s %s %uV, %s from %s, %u segment(s)", g.cols, g.rows,
           g.pixelsPerBin, sb::config::gridPixels(g), sb::config::toString(g.chipset), g.colorOrder, g.voltage,
           sb::config::toString(g.layout), sb::config::toString(g.start), g.segments);
  for (int i = 0; i < sb::config::kLightChannels; i++) {
    ESP_LOGI(TAG, "Light W%d: %s", i + 1, sb::config::toString(cfg.lights[i].mode));
  }
}

// Minimal landing page until the web UI arrives (Task 6).
void registerStatusPage() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    if (network.state() != sb::WifiLink::State::Connected && network.portalActive()) {
      req->redirect("/wifi");
      return;
    }
    String html =
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Smart Bins</title><body style='font:16px system-ui;max-width:420px;margin:24px auto;padding:0 16px'>"
        "<h1>Smart Bins</h1><p>Firmware " SB_FW_VERSION "</p><p>Device: ";
    html += configStore.get().deviceName;
    html += "<br>Uptime: " + String(millis() / 1000) + " s<br>Free heap: " + String(ESP.getFreeHeap()) +
            " bytes</p><p><a href='/wifi'>Wi-Fi settings</a> &middot; <a href='/api/config'>config.json</a></p>";
    req->send(200, "text/html; charset=utf-8", html);
  });

  // Read-only view of the active configuration (editing arrives with the settings UI).
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", sb::config::serialize(configStore.get()).c_str());
  });
}

// BOOT button: hold 5 s to forget Wi-Fi, 10 s for a factory reset (Wi-Fi + config).
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
      network.forgetCredentials();
      delay(200);
      ESP.restart();
    } else if (held >= kWifiResetHoldMs) {
      network.forgetCredentials();
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

  network.begin(configStore.get().deviceName, server);
  registerStatusPage();
  server.begin();
  ESP_LOGI(TAG, "Boot complete, free heap %s", sb::formatBytes(ESP.getFreeHeap()));
}

void loop() {
  network.loop();
  handleButton();
  updateStatusLed();

  static uint32_t lastReport = 0;
  if (millis() - lastReport >= 60000) {
    lastReport = millis();
    ESP_LOGI(TAG, "Uptime %lus, free heap %s, min %s", (unsigned long)(millis() / 1000),
             sb::formatBytes(ESP.getFreeHeap()), sb::formatBytes(ESP.getMinFreeHeap()));
  }
  delay(2);
}
