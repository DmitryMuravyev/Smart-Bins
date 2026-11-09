#pragma once

// WebSocket /ws: pushes changes to every open UI, so a light switched from one
// phone (or the BOOT button, or a script) shows up on all the others at once.
//
// Messages are JSON objects with a "type":
//   {"type":"lights", "version":n, "lights":[...], "stripPower":bool}   on every light change
//   {"type":"leds", "estimateMa":n, "fps":n, "currentLimited":bool, "version":n, "lit":[[x,y],...]}
//       every 2 s, and at once when the lit bins change
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
  // Open WebSocket connections (for the heap log).
  size_t clients() { return ws_.count(); }

 private:
  String lightsMessage();
  String ledsMessage();
  String systemMessage();

  AsyncWebSocket ws_;
  led::WorkLights *lights_ = nullptr;
  led::LedEngine *leds_ = nullptr;
  WifiLink *network_ = nullptr;
  uint32_t sentVersion_ = 0;      // WorkLights::version() at the last lights push
  uint32_t sentMotionVersion_ = 0;  // motionVersion() at the last lights push
  uint32_t sentLedSettings_ = 0;   // ledSettingsVersion() at the last grid push
  uint32_t sentPowerVersion_ = 0;  // WorkLights::powerVersion() at the last lights push
  uint32_t lastLightsPower_ = 0;   // when the lights were last pushed (for the power figures)
  uint32_t sentLedsVersion_ = 0;  // LedEngine::version() at the last grid push
  uint32_t lastStatus_ = 0;
  uint32_t lastCleanup_ = 0;
};

}  // namespace sb
