#pragma once

#include <ArduinoJson.h>

class AsyncWebServer;

namespace sb {
namespace led {
class WorkLights;
}
class MotionSensor;

// Work-light endpoints, used by the Lights page and handy for scripts:
//   GET /api/lights                      all lights and their state
// The two changing endpoints take GET (address bar, scripts) and POST with a
// form-encoded body (the UI uses that).
//   GET /api/lights/set?id=0[&on=1|0|toggle][&level=1..255][&color=FF8800]
//                       [&kelvin=1500..10000][&effect=solid|breathe|rainbow]
//   GET /api/lights/off                  every light off
// A level, color or effect without `on` changes the setting and leaves the
// light on or off as it is (an off light uses it at the next "on"; send on=1
// with it to switch on as well). level=0 switches the light off and keeps the
// previous level for the next "on".
void registerLightsApi(AsyncWebServer &server, led::WorkLights &lights);

// Fills `doc` with {"version": n, "lights": [...], "stripPower": bool}, as returned by
// /api/lights and pushed over the WebSocket. With a motion sensor in use,
// also "motion": {"present": bool, "signal": bool}.
void writeLights(JsonDocument &doc, led::WorkLights &lights);

// The motion sensor whose state writeLights() adds (nullptr: none).
void setMotionSensor(const MotionSensor *sensor);
// MotionSensor::version(), or 0 without one: the WebSocket pushes the lights
// when it moves.
uint32_t motionVersion();

}  // namespace sb
