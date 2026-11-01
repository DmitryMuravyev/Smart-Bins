#include "LiveUpdates.h"

#include <ArduinoJson.h>

#include "LedApi.h"
#include "LightsApi.h"
#include "Security.h"
#include "SystemApi.h"
#include "led/LedEngine.h"
#include "led/WorkLights.h"

namespace sb {

namespace {
// Only debug lines use the tag; below debug log level they compile away,
// so [[maybe_unused]] keeps that build free of warnings.
[[maybe_unused]] const char *TAG = "live";
// How often the slowly changing values (LED current, memory, signal) are pushed.
constexpr uint32_t kStatusPeriodMs = 2000;
// cleanupClients() closes the oldest connections beyond the library's limit;
// once a second is plenty, as a page opens at most one socket.
constexpr uint32_t kCleanupPeriodMs = 1000;
}  // namespace

void LiveUpdates::begin(AsyncWebServer &server, Security &security, led::WorkLights &lights, led::LedEngine &leds,
                        WifiLink &network) {
  lights_ = &lights;
  leds_ = &leds;
  network_ = &network;
  sentVersion_ = lights.version();
  sentLedsVersion_ = leds.version();

  // The password check exempts /ws (a browser can't answer a digest challenge
  // on a WebSocket); the session token from /api/session is checked here instead.
  ws_.handleHandshake([&security](AsyncWebServerRequest *req) { return security.tokenValid(req); });

  // Runs on the network task. A new client gets a full snapshot straight away,
  // so the page never shows stale values while waiting for the next push.
  ws_.onEvent([this](AsyncWebSocket *, AsyncWebSocketClient *client, AwsEventType type, void *, uint8_t *, size_t) {
    if (type == WS_EVT_CONNECT) {
      ESP_LOGD(TAG, "Client %u connected (%u open)", client->id(), (unsigned)ws_.count());
      // A client whose queue fills up (WS_MAX_QUEUED_MESSAGES in
      // platformio.ini) has stopped reading, e.g. a phone with its screen
      // locked. Dropping messages would leave its page showing an old state;
      // closing frees the queue, and the page reconnects and is sent the
      // current state (just below) when it wakes up.
      client->setCloseClientOnQueueFull(true);
      client->text(lightsMessage());
      client->text(ledsMessage());
      client->text(systemMessage());
    } else if (type == WS_EVT_DISCONNECT) {
      ESP_LOGD(TAG, "Client %u left", client->id());
    }
  });
  server.addHandler(&ws_);
}

void LiveUpdates::loop() {
  const uint32_t now = millis();
  if (now - lastCleanup_ >= kCleanupPeriodMs) {
    lastCleanup_ = now;
    ws_.cleanupClients();
  }
  // Nobody listening: skip building messages. The lights version still moves
  // on, and a client that connects later gets a snapshot anyway.
  if (ws_.count() == 0) {
    sentVersion_ = lights_->version();
    sentLedsVersion_ = leds_->version();
    return;
  }

  // Polling a counter instead of a callback from WorkLights: changes arrive
  // from web requests on another task, and several in quick succession (a
  // slider being dragged) collapse into one message per loop pass.
  const uint32_t version = lights_->version();
  if (version != sentVersion_) {
    sentVersion_ = version;
    ws_.textAll(lightsMessage());
  }

  // Lit bins changed (located, cleared, timed out): push the grid status now
  // rather than at the next 2 s tick, so the bin buttons follow at once.
  const uint32_t ledsVersion = leds_->version();
  const bool ledsChanged = ledsVersion != sentLedsVersion_;
  sentLedsVersion_ = ledsVersion;

  if (ledsChanged || now - lastStatus_ >= kStatusPeriodMs) ws_.textAll(ledsMessage());
  if (now - lastStatus_ >= kStatusPeriodMs) {
    lastStatus_ = now;
    ws_.textAll(systemMessage());
  }
}

String LiveUpdates::lightsMessage() {
  JsonDocument doc;
  doc["type"] = "lights";
  writeLights(doc, *lights_);
  String out;
  serializeJson(doc, out);
  return out;
}

String LiveUpdates::ledsMessage() {
  JsonDocument doc;
  doc["type"] = "leds";
  writeLedStatus(doc, *leds_);
  String out;
  serializeJson(doc, out);
  return out;
}

String LiveUpdates::systemMessage() {
  JsonDocument doc;
  doc["type"] = "system";
  writeSystemStatus(doc["system"].to<JsonObject>(), *network_);
  String out;
  serializeJson(doc, out);
  return out;
}

}  // namespace sb
