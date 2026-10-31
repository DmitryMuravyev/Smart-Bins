#pragma once

#include <ArduinoJson.h>

class AsyncWebServer;

namespace sb {

class ConfigStore;
namespace led {
class LedEngine;
}

// Bin grid endpoints. Each changing endpoint takes GET (handy for testing from
// a browser's address bar) and POST with a form-encoded body (used by the UI).
//   /api/locate?x=3&y=2[&color=FF0000][&mode=steady|slow-pulse|fast-pulse][&s=30]
//       lights a bin; anything left out comes from the locate defaults
//   /api/locate/clear[?x=3&y=2]          fades every located bin (or just one) out
//   /api/locate/defaults[?color=..&mode=..&s=..]
//       GET without parameters reads the defaults; with them (or POST) changes
//       and saves them (config.json locate.*), effective at once
//   /api/led/test?pattern=walk|chain|rows|columns|colors|off
//   /api/led/brightness?value=1..255     applied and saved (grid.brightness)
//   GET /api/led/status                  current, frame rate and lit bins
// /api/locate and /api/locate/clear answer with the same status as
// /api/led/status (plus "ok").
void registerLedApi(AsyncWebServer &server, led::LedEngine &engine, ConfigStore &config);

// Fills `doc` with the grid status, as returned by /api/led/status and pushed
// over the WebSocket: powered, test, estimateMa, currentLimited, fps and
// lit ([[x, y], ...]) with its version (LedEngine::version()).
void writeLedStatus(JsonDocument &doc, led::LedEngine &engine);

}  // namespace sb
