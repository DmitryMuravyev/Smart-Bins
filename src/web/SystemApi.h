#pragma once

// Device information, restart and the configuration file:
//   GET  /api/system           firmware, chip, memory, storage, network, password state,
//                              inventory figures (see writeInventoryStats), photo storage
//                              (see writePhotoStats)
//   POST /api/system/restart   restarts the board after the response is sent
//   GET  /api/config           the active configuration (config.json)
//   POST /api/config           replaces it with the JSON body, then restarts to apply it

#include <ArduinoJson.h>

class AsyncWebServer;

namespace sb {

class ConfigStore;
class PhotoStore;
class Security;
class WifiLink;
namespace inventory {
class InventoryDb;
}

// `scheduleRestart` restarts the board a moment later, from the main loop, so
// the response still reaches the browser.
void registerSystemApi(AsyncWebServer &server, ConfigStore &config, WifiLink &network, Security &security,
                       inventory::InventoryDb &inventory, PhotoStore &photos, void (*scheduleRestart)());

// The values of /api/system that change while the board runs (uptime, memory,
// signal strength). The WebSocket pushes these every few seconds.
void writeSystemStatus(JsonObject out, WifiLink &network);

}  // namespace sb
