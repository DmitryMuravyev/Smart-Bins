#include "SystemApi.h"

#include <ESPAsyncWebServer.h>
#include <FFat.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#include "ApiUtil.h"
#include "BootReport.h"
#include "Health.h"
#include "ConfigStore.h"
#include "InventoryApi.h"
#include "MemoryProfile.h"
#include "PhotoApi.h"
#include "Security.h"
#include "PinMap.h"
#include "WifiLink.h"
#include "led/RmtStrip.h"

#ifndef SB_FW_VERSION
#define SB_FW_VERSION "dev"
#endif

namespace sb {

namespace {
// Largest config.json accepted. The real file is around 2 KB; the limit only
// stops a runaway upload from taking the heap.
constexpr size_t kMaxConfigBody = 8192;

// Firmware upload state. Only one upload runs at a time (a second one is
// refused while the first is running); the error, if any, is kept for the
// answer sent when the upload ends.
bool updateRunning = false;
String updateError;

// Every GPIO of the WROOM-32 that a function of `use` may take, by the same
// rules config validation applies (flash, console, GPIO12, input-only).
void writeAllowedPins(JsonArray out, board::PinUse use) {
  for (int g = 0; g <= 39; g++) {
    const board::PinAssignment pin{"x", static_cast<int8_t>(g), use, false};
    if (board::checkPin(pin).error == board::PinError::None) out.add(g);
  }
}
}  // namespace

void writeSystemStatus(JsonObject out, WifiLink &network) {
  out["uptimeS"] = millis() / 1000;
  out["heapFree"] = heapFree();
  out["heapMin"] = heapMin();
  // Why the board last restarted (see Health.h); `restartFault` marks the
  // reasons that point at a problem.
  out["restart"] = health::lastRestart();
  out["restartFault"] = health::lastRestartWasFault();
  // PSRAM, on a board that has it (the System tab shows a row for it).
  if (ESP.getPsramSize() > 0) {
    out["psramFree"] = psramFree();
    out["psramTotal"] = ESP.getPsramSize();
  }
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
                       inventory::InventoryDb &inventory, PhotoStore &photos, void (*scheduleRestart)()) {
  // exact(): /api/system/restart must not land here (see LightsApi.cpp).
  server.on(AsyncURIMatcher::exact("/api/system"), HTTP_GET, [&config, &network, &security, &inventory, &photos](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["firmware"] = SB_FW_VERSION;
    // String copies: ArduinoJson keeps a char array by pointer (it takes it for
    // a literal), and the config buffer could change before serialisation.
    doc["device"] = String(config.get().deviceName);
    doc["language"] = String(config.get().language);
    doc["chip"] = ESP.getChipModel();
    doc["cpuMhz"] = ESP.getCpuFreqMHz();
    // ESP.getFlashChipSize() reads the image header straight from flash
    // (esp_flash_read), which switches the cache off like any file access:
    // with no guard, a frame on the wire at that moment was cut short. The
    // size never changes, so it is read once, under the guard, and kept.
    static const uint32_t flashBytes = [] {
      led::FlashGuard guard;
      return static_cast<uint32_t>(ESP.getFlashChipSize());
    }();
    doc["flashBytes"] = flashBytes;
    {
      // The file system figures read the FAT from flash, which switches the
      // cache off: the strips pause meanwhile (see FlashGuard), or a frame
      // on the wire can run long and light pixels past the end of the grid.
      led::FlashGuard guard;
      doc["fsUsed"] = FFat.usedBytes();
      doc["fsTotal"] = FFat.totalBytes();
      writePhotoStats(doc["photos"].to<JsonObject>(), photos);
    }
    // Outside the guard: the database takes its own (its lock comes first).
    writeInventoryStats(doc["inventory"].to<JsonObject>(), inventory);
    doc["passwordSet"] = security.passwordSet();
    // Firmware upload from the browser needs a second app slot (8 and 16 MB
    // boards); the 4 MB layout has only one and is updated over USB.
    doc["firmwareUpload"] = esp_ota_get_next_update_partition(nullptr) != nullptr;
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

  // The pin map for the Hardware page: every function with its configured
  // and default GPIO, and the GPIOs an output or an input may use. Changes
  // are saved through POST /api/config (pins.*), which checks them again.
  server.on(AsyncURIMatcher::exact("/api/pins"), HTTP_GET, [&config](AsyncWebServerRequest *req) {
    JsonDocument doc;
    JsonArray pins = doc["pins"].to<JsonArray>();
    for (size_t i = 0; i < board::kDefaultPinCount; i++) {
      const board::PinAssignment &d = board::kDefaultPins[i];
      JsonObject p = pins.add<JsonObject>();
      p["name"] = d.name;
      p["gpio"] = config.get().pins.gpio[i];
      p["default"] = d.gpio;
      p["input"] = d.use == board::PinUse::Input;
      if (board::isFixedPin(d.name)) p["fixed"] = true;
    }
    // False on boards without PSRAM: the microphone is off there whatever the
    // setting says (kMicrophone in MemoryProfile.h), so the page greys it out.
    doc["micAvailable"] = mem::kMicrophone;
    writeAllowedPins(doc["outputs"].to<JsonArray>(), board::PinUse::Output);
    writeAllowedPins(doc["inputs"].to<JsonArray>(), board::PinUse::Input);
    sendJson(req, 200, doc);
  });

  // Firmware update from the browser: a multipart upload of the .bin file
  // (field "firmware"). It is written to the other app slot as it arrives;
  // when it is complete and checks out, the board restarts into it. Anything
  // wrong (no second slot, not a firmware image, upload cut short) leaves the
  // running firmware as it is.
  server.on(
      AsyncURIMatcher::exact("/api/firmware"), HTTP_POST,
      [scheduleRestart](AsyncWebServerRequest *req) {
        if (updateRunning) {  // the body ended without the upload's last part
          Update.abort();
          updateRunning = false;
          if (updateError.isEmpty()) updateError = "upload incomplete";
        }
        if (!updateError.isEmpty() || !Update.isFinished()) {
          return sendError(req, 400, updateError.isEmpty() ? "no firmware file received" : updateError.c_str());
        }
        ESP_LOGW("system", "Firmware update written, restarting");
        scheduleRestart();
        JsonDocument doc;
        doc["ok"] = true;
        doc["restarting"] = true;
        sendJson(req, 200, doc);
      },
      [](AsyncWebServerRequest *req, const String &filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (index == 0) {
          if (updateRunning) Update.abort();  // a previous upload that was cut off
          updateError = "";
          updateRunning = true;
          ESP_LOGW("system", "Firmware upload: %s", filename.c_str());
          led::FlashGuard guard;  // begin() erases the first sectors
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            updateError = Update.errorString();
            updateRunning = false;
            return;
          }
        }
        if (!updateRunning) return;  // failed earlier: ignore the rest
        if (len) {
          // Writing flash switches the cache off; the guard keeps the LED
          // strips from flashing random colors meanwhile (see RmtStrip.h).
          led::FlashGuard guard;
          if (Update.write(data, len) != len) {
            updateError = Update.errorString();
            Update.abort();
            updateRunning = false;
            return;
          }
        }
        if (final) {
          led::FlashGuard guard;
          // end(true): the size is what arrived; the image is checked (header
          // and checksum) and the boot slot switched only if it is valid.
          if (!Update.end(true)) updateError = Update.errorString();
          updateRunning = false;
        }
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
