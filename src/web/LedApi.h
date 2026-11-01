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
//   /api/effects[?effect=off|fire|sparks|relax|demo|spectrum|beat|pulse][&speed=1..255]
//       [&intensity=1..255][&palette=auto|heat|ocean|forest|lava|rainbow|party]
//       [&brightness=1..255][&timer=0..1440]
//       starts an effect and/or changes its parameters (saved as effects.*);
//       answers with the effect that runs, its parameters, remainingS and
//       microphone (whether one was found). The music effects (spectrum,
//       beat, pulse) answer 409 without a microphone.
//   GET /api/led/status                  current, frame rate, lit bins and the effect
// /api/locate and /api/locate/clear answer with the same status as
// /api/led/status (plus "ok").
void registerLedApi(AsyncWebServer &server, led::LedEngine &engine, ConfigStore &config);

// Fills `doc` with the grid status, as returned by /api/led/status and pushed
// over the WebSocket: powered, test, estimateMa, currentLimited, fps, effect,
// effectRemainingS and lit ([[x, y], ...]) with its version
// (LedEngine::version()).
void writeLedStatus(JsonDocument &doc, led::LedEngine &engine);

}  // namespace sb
