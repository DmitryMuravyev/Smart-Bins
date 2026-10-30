#pragma once

class AsyncWebServer;

namespace sb {
namespace led {
class WorkLights;
}

// Temporary work-light endpoints for testing until the web UI (Task 6):
//   GET /api/lights                      all lights and their state
//   GET /api/lights/set?id=0[&on=1|0|toggle][&level=1..255][&color=FF8800]
//                       [&kelvin=1500..10000][&effect=solid|breathe|rainbow]
//   GET /api/lights/off                  every light off
// Setting a level, colour or effect without `on` also switches the light on;
// level=0 switches it off and keeps the previous level for the next "on".
void registerLightsApi(AsyncWebServer &server, led::WorkLights &lights);

}  // namespace sb
