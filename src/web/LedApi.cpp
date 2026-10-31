#include "LedApi.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include "ApiUtil.h"
#include "ConfigStore.h"
#include "led/LedEngine.h"

namespace sb {

namespace {

// Reads the optional locate settings (color, mode, s) into `out`, leaving the
// ones that are not given as they are. Returns an error message or nullptr.
const char *readLocateParams(AsyncWebServerRequest *req, config::LocateConfig &out) {
  if (hasParam(req, "color") && !colorParam(req, "color", out.color)) return "color must be 6 hex digits, e.g. FF0000";
  if (const AsyncWebParameter *p = param(req, "mode")) {
    const String &m = p->value();
    // Same names as locate.mode in config.json.
    if (m == "steady") out.mode = config::LocateMode::Steady;
    else if (m == "slow-pulse") out.mode = config::LocateMode::SlowPulse;
    else if (m == "fast-pulse") out.mode = config::LocateMode::FastPulse;
    else return "mode must be steady, slow-pulse or fast-pulse";
  }
  long seconds;
  if (hasParam(req, "s")) {
    if (!intParam(req, "s", 0, 3600, seconds)) return "s must be 0..3600 (0 = until cleared)";
    out.durationS = static_cast<uint16_t>(seconds);
  }
  return nullptr;
}

void sendLocateDefaults(AsyncWebServerRequest *req, const config::LocateConfig &d) {
  JsonDocument doc;
  char hex[8];
  snprintf(hex, sizeof(hex), "%06lX", static_cast<unsigned long>(d.color & 0xFFFFFF));
  doc["color"] = static_cast<const char *>(hex);  // cast: copy the text, see LightsApi.cpp
  doc["mode"] = config::toString(d.mode);
  doc["s"] = d.durationS;
  sendJson(req, 200, doc);
}

}  // namespace

void registerLedApi(AsyncWebServer &server, led::LedEngine &engine, ConfigStore &config) {
  // exact(): otherwise this route also catches "/api/locate/clear" (see LightsApi.cpp).
  server.on(AsyncURIMatcher::exact("/api/locate"), HTTP_GET | HTTP_POST, [&engine, &config](AsyncWebServerRequest *req) {
    const auto &g = engine.mapper().geometry();
    long x, y;
    if (!intParam(req, "x", 0, g.cols - 1, x) || !intParam(req, "y", 0, g.rows - 1, y)) {
      return sendError(req, 400, "x and y must be inside the grid");
    }
    // Anything not given comes from the configured locate defaults.
    config::LocateConfig opts = config.get().locate;
    if (const char *err = readLocateParams(req, opts)) return sendError(req, 400, err);
    const bool ok = engine.locate(x, y, led::Rgb::fromHex(opts.color), led::pulseMode(opts.mode),
                                  static_cast<uint32_t>(opts.durationS) * 1000);
    // The answer carries the new grid status (lit bins and their version), so
    // the page can show the result without waiting for the WebSocket.
    JsonDocument doc;
    doc["ok"] = ok;
    writeLedStatus(doc, engine);
    sendJson(req, ok ? 200 : 400, doc);
  });

  // The default colour, mode and duration of a locate. A change applies at
  // once and is saved to config.json (locate.*), no restart needed.
  server.on("/api/locate/defaults", HTTP_GET | HTTP_POST, [&engine, &config](AsyncWebServerRequest *req) {
    const bool change = hasParam(req, "color") || hasParam(req, "mode") || hasParam(req, "s");
    if (!change) return sendLocateDefaults(req, config.get().locate);
    config::AppConfig cfg = config.get();
    if (const char *err = readLocateParams(req, cfg.locate)) return sendError(req, 400, err);
    std::string err;
    if (!config.save(cfg, err)) return sendError(req, 500, err.c_str());
    engine.setLocateDefaults(cfg.locate);
    sendLocateDefaults(req, cfg.locate);
  });

  // Without x and y every located bin fades out; with them, just that one.
  server.on("/api/locate/clear", HTTP_GET | HTTP_POST, [&engine](AsyncWebServerRequest *req) {
    if (hasParam(req, "x") || hasParam(req, "y")) {
      const auto &g = engine.mapper().geometry();
      long x, y;
      if (!intParam(req, "x", 0, g.cols - 1, x) || !intParam(req, "y", 0, g.rows - 1, y)) {
        return sendError(req, 400, "x and y must be inside the grid");
      }
      engine.clearLocate(x, y);
    } else {
      engine.clearLocates();
    }
    // With the new grid status, as for /api/locate.
    JsonDocument doc;
    doc["ok"] = true;
    writeLedStatus(doc, engine);
    sendJson(req, 200, doc);
  });

  server.on("/api/led/test", HTTP_GET | HTTP_POST, [&engine](AsyncWebServerRequest *req) {
    led::TestPattern p;
    if (!hasParam(req, "pattern") || !led::parseTestPattern(param(req, "pattern")->value().c_str(), p)) {
      return sendError(req, 400, "pattern must be walk, chain, rows, columns, colors or off");
    }
    engine.startTest(p);
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // Applies at once (with a fade) and is saved as grid.brightness. The UI
  // sends it when the slider is released, so a drag costs one flash write.
  server.on("/api/led/brightness", HTTP_GET | HTTP_POST, [&engine, &config](AsyncWebServerRequest *req) {
    long v;
    if (!intParam(req, "value", 1, 255, v)) return sendError(req, 400, "value must be 1..255");
    engine.setBrightness(static_cast<uint8_t>(v));
    config::AppConfig cfg = config.get();
    if (cfg.grid.brightness != v) {
      cfg.grid.brightness = static_cast<uint8_t>(v);
      std::string err;
      if (!config.save(cfg, err)) return sendError(req, 500, err.c_str());
    }
    req->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/led/status", HTTP_GET, [&engine](AsyncWebServerRequest *req) {
    JsonDocument doc;
    writeLedStatus(doc, engine);
    sendJson(req, 200, doc);
  });
}

void writeLedStatus(JsonDocument &doc, led::LedEngine &engine) {
  const led::LedEngine::Status s = engine.status();
  doc["powered"] = s.powered;
  doc["test"] = led::toString(s.test);
  doc["estimateMa"] = s.estimateMa;
  doc["currentLimited"] = s.limitLevel < 255;
  doc["fps"] = s.fps;
  // Lit bins as [x, y] pairs, so every open page shows the same highlights.
  led::LedEngine::Bin bins[led::LedEngine::kMaxHighlights];
  uint32_t version;
  const uint8_t n = engine.litBins(bins, led::LedEngine::kMaxHighlights, &version);
  // The page drops a status older than one it has already shown: a push built
  // just before a bin was toggled would otherwise undo the toggle on screen
  // for a moment.
  doc["version"] = version;
  JsonArray lit = doc["lit"].to<JsonArray>();
  for (uint8_t i = 0; i < n; i++) {
    JsonArray b = lit.add<JsonArray>();
    b.add(bins[i].x);
    b.add(bins[i].y);
  }
}

}  // namespace sb
