#pragma once

class AsyncWebServer;

namespace sb {
namespace led {
class LedEngine;
}

// Temporary LED endpoints for testing until the web UI arrives (Task 6):
//   GET /api/locate?x=3&y=2[&color=FF0000][&mode=steady|slow-pulse|fast-pulse][&s=30]
//   GET /api/locate/clear
//   GET /api/led/test?pattern=walk|chain|rows|columns|colors|off
//   GET /api/led/brightness?value=0..255
//   GET /api/led/status
void registerLedApi(AsyncWebServer &server, led::LedEngine &engine);

}  // namespace sb
