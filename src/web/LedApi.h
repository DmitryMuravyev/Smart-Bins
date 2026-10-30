#pragma once

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
//   /api/locate/clear                    fades every located bin out
//   /api/locate/defaults[?color=..&mode=..&s=..]
//       GET without parameters reads the defaults; with them (or POST) changes
//       and saves them (config.json locate.*), effective at once
//   /api/led/test?pattern=walk|chain|rows|columns|colors|off
//   /api/led/brightness?value=0..255     until the next restart
//   GET /api/led/status
void registerLedApi(AsyncWebServer &server, led::LedEngine &engine, ConfigStore &config);

}  // namespace sb
