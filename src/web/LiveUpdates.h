#pragma once

// WebSocket /ws: pushes changes to every open UI, so a light switched from one
// phone (or the BOOT button, or a script) shows up on all the others at once.
//
// Messages are JSON objects with a "type":
//   {"type":"lights", "lights":[...], "stripPower":bool}   on every light change
//   {"type":"leds", "estimateMa":n, "fps":n, "currentLimited":bool}   every 2 s
//   {"type":"system", "system":{uptimeS, heapFree, heapMin, wifi{...}}}   every 2 s
// A new client gets all three at once. The socket only pushes; changes are
// made through the HTTP API.

#include <ESPAsyncWebServer.h>

namespace sb {

class Security;
class WifiLink;
namespace led {
class LedEngine;
class WorkLights;
}  // namespace led

class LiveUpdates {
 public:
  LiveUpdates() : ws_("/ws") {}

  void begin(AsyncWebServer &server, Security &security, led::WorkLights &lights, led::LedEngine &leds,
             WifiLink &network);
  // Call from the main loop: sends what changed and drops dead connections.
  void loop();

 private:
  String lightsMessage();
  String ledsMessage();
  String systemMessage();

  AsyncWebSocket ws_;
  led::WorkLights *lights_ = nullptr;
  led::LedEngine *leds_ = nullptr;
  WifiLink *network_ = nullptr;
  uint32_t sentVersion_ = 0;  // WorkLights::version() at the last lights push
  uint32_t lastStatus_ = 0;
  uint32_t lastCleanup_ = 0;
};

}  // namespace sb
