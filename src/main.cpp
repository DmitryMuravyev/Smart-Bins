// Smart Bins firmware entry point.
//
// Boot order: hold LED outputs low, print the boot report, mount LittleFS,
// load the configuration, start the LED engine, then bring up Wi-Fi (or the
// setup portal), mDNS, over-the-air updates and the web server.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>

#include "BootReport.h"
#include "ConfigStore.h"
#include "PinMap.h"
#include "WifiLink.h"
#include "led/LedEngine.h"
#include "web/LedApi.h"

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
AsyncWebServer server(80);
uint32_t restartAt = 0;  // set by the config endpoint; restart happens in loop()

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
            " bytes</p><p><a href='/wifi'>Wi-Fi settings</a> &middot; <a href='/config'>edit config</a> &middot; "
            "<a href='/api/led/status'>LED status</a></p>"
            "<h2>LED tests</h2><p>"
            "<a href='/api/led/test?pattern=walk'>walk bins</a> &middot; "
            "<a href='/api/led/test?pattern=chain'>chain</a> &middot; "
            "<a href='/api/led/test?pattern=rows'>rows</a> &middot; "
            "<a href='/api/led/test?pattern=columns'>columns</a> &middot; "
            "<a href='/api/led/test?pattern=colors'>colours</a> &middot; "
            "<a href='/api/led/test?pattern=off'>stop</a></p>"
            "<form action='/api/locate'>Locate bin x <input name=x size=2 value=0> y <input name=y size=2 value=0> "
            "<button>Light</button> <a href='/api/locate/clear'>clear</a></form>";
    req->send(200, "text/html; charset=utf-8", html);
  });

  // Read-only view of the active configuration (editing arrives with the settings UI).
  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", sb::config::serialize(configStore.get()).c_str());
  });

  // Replaces the whole configuration with the posted JSON, then restarts to apply it.
  // Temporary until the settings UI (Task 14); /config is a plain editor for it.
  server.on(
      "/api/config", HTTP_POST,
      [](AsyncWebServerRequest *req) {
        // The body is collected with malloc() because the server free()s _tempObject on aborted requests.
        char *body = static_cast<char *>(req->_tempObject);
        if (!body) return req->send(400, "application/json", "{\"error\":\"empty or too large body\"}");
        sb::config::AppConfig cfg;
        std::string err;
        const bool ok = sb::config::parse(body, cfg, err) && configStore.save(cfg, err);
        free(body);
        req->_tempObject = nullptr;
        if (!ok) {
          JsonDocument doc;
          doc["error"] = err;
          String out;
          serializeJson(doc, out);
          return req->send(400, "application/json", out);
        }
        restartAt = millis() + 1000;
        req->send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
      },
      nullptr,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        if (total > 8192) return;
        if (index == 0) req->_tempObject = calloc(total + 1, 1);
        char *body = static_cast<char *>(req->_tempObject);
        if (body && index + len <= total) memcpy(body + index, data, len);
      });

  server.on("/config", HTTP_GET, [](AsyncWebServerRequest *req) {
    String html =
        "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Smart Bins config</title><body style='font:16px system-ui;max-width:640px;margin:24px auto;padding:0 16px'>"
        "<h1>config.json</h1><p>Edit and save; the board restarts to apply. Invalid values are rejected with the field "
        "name.</p><textarea id=c spellcheck=false style='width:100%;height:60vh;font:13px monospace'></textarea>"
        "<p><button id=s>Save and restart</button> <span id=m></span></p><script>"
        "fetch('/api/config').then(r=>r.text()).then(t=>c.value=t);"
        "s.onclick=async()=>{const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},"
        "body:c.value});const j=await r.json();m.textContent=r.ok?'Saved, restarting...':'Error: '+j.error;"
        "if(r.ok)setTimeout(()=>location.reload(),6000)};</script>";
    req->send(200, "text/html; charset=utf-8", html);
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

  leds.begin(configStore.get(), sb::board::defaultPin("grid_data_a"), sb::board::defaultPin("grid_data_b"),
             sb::board::defaultPin("grid_power"));

  network.begin(configStore.get().deviceName, server);
  registerStatusPage();
  sb::registerLedApi(server, leds);
  server.begin();
  ESP_LOGI(TAG, "Boot complete, free heap %s", sb::formatBytes(ESP.getFreeHeap()));
}

void loop() {
  if (restartAt && (int32_t)(millis() - restartAt) >= 0) ESP.restart();
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
