#include "LedApi.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include "ApiUtil.h"
#include "led/LedEngine.h"

namespace sb {

void registerLedApi(AsyncWebServer &server, led::LedEngine &engine) {
  server.on("/api/locate", HTTP_GET, [&engine](AsyncWebServerRequest *req) {
    const auto &g = engine.mapper().geometry();
    long x, y;
    if (!intParam(req, "x", 0, g.cols - 1, x) || !intParam(req, "y", 0, g.rows - 1, y)) {
      return sendError(req, 400, "x and y must be inside the grid");
    }
    bool custom = req->hasParam("color") || req->hasParam("mode") || req->hasParam("s");
    bool ok;
    if (!custom) {
      ok = engine.locate(x, y);
    } else {
      uint32_t color = 0x00FF40;
      if (req->hasParam("color") && !colorParam(req, "color", color)) {
        return sendError(req, 400, "color must be 6 hex digits, e.g. FF0000");
      }
      led::PulseMode mode = led::PulseMode::Steady;
      if (req->hasParam("mode")) {
        const String &m = req->getParam("mode")->value();
        if (m == "slow-pulse") mode = led::PulseMode::Slow;
        else if (m == "fast-pulse") mode = led::PulseMode::Fast;
        else if (m != "steady") return sendError(req, 400, "mode must be steady, slow-pulse or fast-pulse");
      }
      long seconds = 30;
      if (req->hasParam("s") && !intParam(req, "s", 0, 3600, seconds)) {
        return sendError(req, 400, "s must be 0..3600 (0 = until cleared)");
      }
      ok = engine.locate(x, y, led::Rgb::fromHex(color), mode, static_cast<uint32_t>(seconds) * 1000);
    }
    JsonDocument doc;
    doc["ok"] = ok;
    sendJson(req, ok ? 200 : 400, doc);
  });

  server.on("/api/locate/clear", HTTP_GET, [&engine](AsyncWebServerRequest *req) {
    engine.clearLocates();
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/led/test", HTTP_GET, [&engine](AsyncWebServerRequest *req) {
    led::TestPattern p;
    if (!req->hasParam("pattern") || !led::parseTestPattern(req->getParam("pattern")->value().c_str(), p)) {
      return sendError(req, 400, "pattern must be walk, chain, rows, columns, colors or off");
    }
    engine.startTest(p);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/led/brightness", HTTP_GET, [&engine](AsyncWebServerRequest *req) {
    long v;
    if (!intParam(req, "value", 0, 255, v)) return sendError(req, 400, "value must be 0..255");
    engine.setBrightness(static_cast<uint8_t>(v));
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/led/status", HTTP_GET, [&engine](AsyncWebServerRequest *req) {
    const led::LedEngine::Status s = engine.status();
    JsonDocument doc;
    doc["powered"] = s.powered;
    doc["highlights"] = s.highlights;
    doc["test"] = led::toString(s.test);
    doc["estimateMa"] = s.estimateMa;
    doc["currentLimited"] = s.limitLevel < 255;
    doc["fps"] = s.fps;
    sendJson(req, 200, doc);
  });
}

}  // namespace sb
