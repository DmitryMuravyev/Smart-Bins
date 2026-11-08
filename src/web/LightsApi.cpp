#include "LightsApi.h"

#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include "ApiUtil.h"
#include "MotionSensor.h"
#include "led/WorkLights.h"

namespace sb {

namespace {

void addLight(JsonArray arr, const led::WorkLights::Info &info) {
  JsonObject o = arr.add<JsonObject>();
  o["id"] = info.id;
  // The cast matters: ArduinoJson stores a `const char[N]` by pointer, as if
  // it were a string literal, instead of copying it. sendLights() reuses one
  // Info for every light, so without the copy every entry would end up
  // showing the last light's name.
  o["name"] = static_cast<const char *>(info.name);
  o["type"] = config::toString(info.type);
  o["on"] = info.state.on;
  o["level"] = info.state.level;
  if (info.type != config::LightMode::White) {
    char color[8];
    snprintf(color, sizeof(color), "#%02X%02X%02X", info.state.color.r, info.state.color.g, info.state.color.b);
    o["color"] = color;
  }
  o["effect"] = led::toString(info.state.effect);
  if (info.type == config::LightMode::Addressable) {
    o["pixels"] = info.pixels;
    o["estimateMa"] = info.estimateMa;
    o["currentLimited"] = info.currentLimited;
  }
}

}  // namespace

namespace {
MotionSensor *motionSensor = nullptr;
}  // namespace

void setMotionSensor(MotionSensor *sensor) { motionSensor = sensor; }

void wakeMotionSensor() {
  if (motionSensor && motionSensor->enabled()) motionSensor->wake();
}

uint32_t motionVersion() { return motionSensor && motionSensor->enabled() ? motionSensor->version() : 0; }

void writeLights(JsonDocument &doc, led::WorkLights &lights) {
  // The change counter, read before the lights themselves: a change made in
  // between then shows up in the list under the older number, which is safe
  // (the page may skip it, and the change's own push follows). Reading it
  // after could label an old list with a new number, and the page would keep
  // showing that old state. The page drops any list numbered below one it has
  // already shown, so a push built just before a change can't undo it on
  // screen.
  doc["version"] = lights.version();
  JsonArray arr = doc["lights"].to<JsonArray>();
  led::WorkLights::Info info;
  for (uint8_t i = 0; i < lights.count(); i++) {
    if (lights.info(i, info)) addLight(arr, info);
  }
  doc["stripPower"] = lights.stripPower();
  if (motionSensor && motionSensor->enabled()) {
    JsonObject m = doc["motion"].to<JsonObject>();
    m["present"] = motionSensor->present();
    m["signal"] = motionSensor->signal();
  }
}

namespace {

void sendLights(AsyncWebServerRequest *req, led::WorkLights &lights) {
  JsonDocument doc;
  writeLights(doc, lights);
  sendJson(req, 200, doc);
}

}  // namespace

void registerLightsApi(AsyncWebServer &server, led::WorkLights &lights) {
  // exact(): a plain "/api/lights" route would also match "/api/lights/set" and
  // "/api/lights/off" (the library treats a path as a prefix of "path/..."),
  // and as the first route registered it would answer them with the list.
  server.on(AsyncURIMatcher::exact("/api/lights"), HTTP_GET,
            [&lights](AsyncWebServerRequest *req) { sendLights(req, lights); });

  server.on("/api/lights/off", HTTP_GET | HTTP_POST, [&lights](AsyncWebServerRequest *req) {
    lights.allOff();
    sendLights(req, lights);
  });

  server.on("/api/lights/set", HTTP_GET | HTTP_POST, [&lights](AsyncWebServerRequest *req) {
    long id;
    if (lights.count() == 0) return sendError(req, 404, "no work lights are configured (see lights in /config)");
    if (!intParam(req, "id", 0, lights.count() - 1, id)) return sendError(req, 400, "id must be a light number");

    // Every parameter is checked before anything changes, so a bad request
    // never leaves the light half-updated.
    led::WorkLights::Change c;
    if (hasParam(req, "on")) {
      const String &v = param(req, "on")->value();
      if (v == "1" || v == "true") c.power = led::WorkLights::Change::Power::On;
      else if (v == "0" || v == "false") c.power = led::WorkLights::Change::Power::Off;
      else if (v == "toggle") c.power = led::WorkLights::Change::Power::Toggle;
      else return sendError(req, 400, "on must be 1, 0 or toggle");
    }
    long n;
    if (hasParam(req, "level")) {
      if (!intParam(req, "level", 0, 255, n)) return sendError(req, 400, "level must be 0..255");
      c.hasLevel = true;
      c.level = static_cast<uint8_t>(n);
    }
    // A color picker and a white-tone slider may both be on the same form;
    // an explicit color wins over kelvin.
    uint32_t rgb;
    if (hasParam(req, "color")) {
      if (!colorParam(req, "color", rgb)) return sendError(req, 400, "color must be 6 hex digits, e.g. FF8800");
      c.hasColor = true;
      c.color = led::Rgb::fromHex(rgb);
    } else if (hasParam(req, "kelvin")) {
      if (!intParam(req, "kelvin", led::kMinKelvin, led::kMaxKelvin, n)) {
        return sendError(req, 400, "kelvin must be 1500..10000");
      }
      c.hasColor = true;
      c.color = led::kelvinToRgb(static_cast<uint16_t>(n));
    }
    if (hasParam(req, "effect")) {
      if (!led::parseWorkEffect(param(req, "effect")->value().c_str(), c.effect)) {
        return sendError(req, 400, "effect must be solid, breathe or rainbow");
      }
      c.hasEffect = true;
    }

    if (const char *err = lights.set(static_cast<uint8_t>(id), c)) return sendError(req, 400, err);
    // Switched on (or left on with a new setting) by someone at a page.
    led::WorkLights::Info info;
    if (lights.info(static_cast<uint8_t>(id), info) && info.state.on) wakeMotionSensor();
    sendLights(req, lights);
  });
}

}  // namespace sb
