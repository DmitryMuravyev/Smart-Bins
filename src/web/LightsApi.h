#pragma once

#include <ArduinoJson.h>

class AsyncWebServer;

namespace sb {
namespace led {
class WorkLights;
}

// Work-light endpoints, used by the Lights page and handy for scripts:
//   GET /api/lights                      all lights and their state
// The two changing endpoints take GET (address bar, scripts) and POST with a
// form-encoded body (the UI uses that).
//   GET /api/lights/set?id=0[&on=1|0|toggle][&level=1..255][&color=FF8800]
//                       [&kelvin=1500..10000][&effect=solid|breathe|rainbow]
//   GET /api/lights/off                  every light off
// Setting a level, colour or effect without `on` also switches the light on;
// level=0 switches it off and keeps the previous level for the next "on".
void registerLightsApi(AsyncWebServer &server, led::WorkLights &lights);

// Fills `doc` with {"version": n, "lights": [...], "stripPower": bool}, as returned by
// /api/lights and pushed over the WebSocket.
void writeLights(JsonDocument &doc, led::WorkLights &lights);

}  // namespace sb
