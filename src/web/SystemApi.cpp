#include "SystemApi.h"

#include <ESPAsyncWebServer.h>
#include <FFat.h>
#include <WiFi.h>

#include "ApiUtil.h"
#include "BootReport.h"
#include "ConfigStore.h"
#include "InventoryApi.h"
#include "Security.h"
#include "WifiLink.h"

#ifndef SB_FW_VERSION
#define SB_FW_VERSION "dev"
#endif

namespace sb {

namespace {
// Largest config.json accepted. The real file is around 2 KB; the limit only
// stops a runaway upload from taking the heap.
constexpr size_t kMaxConfigBody = 8192;
}  // namespace

void writeSystemStatus(JsonObject out, WifiLink &network) {
  out["uptimeS"] = millis() / 1000;
  out["heapFree"] = heapFree();
  out["heapMin"] = heapMin();
  JsonObject wifi = out["wifi"].to<JsonObject>();
  const String ssid = network.ssid();  // empty while not connected
  wifi["ssid"] = ssid;
  wifi["rssi"] = ssid.isEmpty() ? 0 : WiFi.RSSI();
  wifi["ip"] = network.ipAddress();
  // The rest of the current address setup, so Settings can offer it as the
  // starting point for a fixed address.
  if (!ssid.isEmpty()) {
    wifi["gateway"] = WiFi.gatewayIP().toString();
    wifi["subnet"] = WiFi.subnetMask().toString();
    wifi["dns"] = WiFi.dnsIP().toString();
  }
  wifi["host"] = String(network.hostName()) + ".local";
}

void registerSystemApi(AsyncWebServer &server, ConfigStore &config, WifiLink &network, Security &security,
                       inventory::InventoryDb &inventory, void (*scheduleRestart)()) {
  // exact(): /api/system/restart must not land here (see LightsApi.cpp).
  server.on(AsyncURIMatcher::exact("/api/system"), HTTP_GET, [&config, &network, &security, &inventory](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["firmware"] = SB_FW_VERSION;
    // String copies: ArduinoJson keeps a char array by pointer (it takes it for
    // a literal), and the config buffer could change before serialisation.
    doc["device"] = String(config.get().deviceName);
    doc["language"] = String(config.get().language);
    doc["chip"] = ESP.getChipModel();
    doc["cpuMhz"] = ESP.getCpuFreqMHz();
    doc["flashBytes"] = ESP.getFlashChipSize();
    doc["fsUsed"] = FFat.usedBytes();
    doc["fsTotal"] = FFat.totalBytes();
    writeInventoryStats(doc["inventory"].to<JsonObject>(), inventory);
    doc["passwordSet"] = security.passwordSet();
    writeSystemStatus(doc.as<JsonObject>(), network);
    sendJson(req, 200, doc);
  });

  server.on(AsyncURIMatcher::exact("/api/system/restart"), HTTP_POST, [scheduleRestart](AsyncWebServerRequest *req) {
    scheduleRestart();
    JsonDocument doc;
    doc["ok"] = true;
    doc["restarting"] = true;
    sendJson(req, 200, doc);
  });

  server.on(AsyncURIMatcher::exact("/api/config"), HTTP_GET, [&config](AsyncWebServerRequest *req) {
    req->send(200, "application/json", config::serialize(config.get()).c_str());
  });

  // Replaces the whole configuration with the posted JSON, then restarts to
  // apply it. The body arrives in chunks (the third handler below), collected
  // into one buffer; the first handler runs once it is complete.
  server.on(
      AsyncURIMatcher::exact("/api/config"), HTTP_POST,
      [&config, scheduleRestart](AsyncWebServerRequest *req) {
        // calloc'd rather than new'd: the server free()s _tempObject itself when
        // a request is aborted, so it must come from the C allocator.
        char *body = static_cast<char *>(req->_tempObject);
        if (!body) return sendError(req, 400, "empty or too large body");
        config::AppConfig cfg;
        std::string err;
        const bool ok = config::parse(body, cfg, err) && config.save(cfg, err);
        free(body);
        req->_tempObject = nullptr;
        if (!ok) return sendError(req, 400, err.c_str());
        scheduleRestart();
        JsonDocument doc;
        doc["ok"] = true;
        doc["restarting"] = true;
        sendJson(req, 200, doc);
      },
      nullptr,
      [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        // Too large: never allocate, so the handler above answers "too large".
        if (total > kMaxConfigBody) return;
        // +1 and calloc: the buffer is always NUL-terminated for the parser.
        if (index == 0) req->_tempObject = calloc(total + 1, 1);
        char *body = static_cast<char *>(req->_tempObject);
        if (body && index + len <= total) memcpy(body + index, data, len);
      });
}

}  // namespace sb
