#include "Security.h"

#include <ESPAsyncWebServer.h>
#include <MD5Builder.h>
#include <Preferences.h>
#include <WebAuthentication.h>
#include <WiFi.h>
#include <esp_random.h>

#include "ApiUtil.h"
#include "led/RmtStrip.h"

namespace sb {

namespace {
const char *TAG = "security";
const char *kPrefsNamespace = "security";
const char *kUser = "admin";
// Part of the digest hash: changing it invalidates stored passwords.
const char *kRealm = "smartbins";
constexpr size_t kMinLength = 4;
constexpr size_t kMaxLength = 64;
}  // namespace

void Security::begin(AsyncWebServer &server) {
  Preferences prefs;
  // Read-write, so the namespace is created instead of logging an error on first boot.
  if (prefs.begin(kPrefsNamespace, false)) {
    digestHash_ = prefs.getString("digest", "");
    otaHash_ = prefs.getString("ota", "");
    prefs.end();
  }
  newToken();
  ESP_LOGI(TAG, "Web password %s", passwordSet() ? "set" : "not set");

  // Runs before every route. Order of checks: no password -> allow; exempt
  // paths -> allow; otherwise a valid digest response is required, and a
  // missing or wrong one gets a 401 that makes the browser ask.
  server.addMiddleware([this](AsyncWebServerRequest *req, ArMiddlewareNext next) {
    if (!passwordSet() || exempt(req) || req->authenticate(kUser, digestHash_.c_str(), kRealm, true)) {
      next();
      return;
    }
    req->requestAuthentication(AsyncAuthType::AUTH_DIGEST, kRealm);
  });

  server.on(AsyncURIMatcher::exact("/api/session"), HTTP_GET, [this](AsyncWebServerRequest *req) {
    JsonDocument doc;
    doc["token"] = passwordSet() ? token_ : "";
    sendJson(req, 200, doc);
  });

  server.on(AsyncURIMatcher::exact("/api/security"), HTTP_POST, [this](AsyncWebServerRequest *req) {
    const AsyncWebParameter *p = param(req, "password");
    if (!p) return sendError(req, 400, "password missing (empty removes it)");
    const String &pw = p->value();
    if (pw.length() > 0 && (pw.length() < kMinLength || pw.length() > kMaxLength)) {
      return sendError(req, 400, "password must be 4 to 64 characters");
    }
    setPassword(pw);
    JsonDocument doc;
    doc["ok"] = true;
    doc["passwordSet"] = passwordSet();
    sendJson(req, 200, doc);
  });
}

// Requests that skip the password check.
bool Security::exempt(AsyncWebServerRequest *req) const {
  const String &url = req->url();
  // Checked by tokenValid() in the WebSocket handshake instead.
  if (url == "/ws") return true;
  // Arrived through the setup access point (its own IP, not the home network)?
  // Then the Wi-Fi setup page and anything outside /api/ are open: the setup
  // page needs its API, and phones probe random URLs to detect the captive
  // portal. The rest of the API stays protected there too.
  if (req->client() && (WiFi.getMode() & WIFI_AP) && req->client()->localIP() == WiFi.softAPIP()) {
    return url.startsWith("/api/wifi") || !url.startsWith("/api/");
  }
  return false;
}

bool Security::tokenValid(AsyncWebServerRequest *req) const {
  if (!passwordSet()) return true;
  const AsyncWebParameter *p = req->getParam("token");
  return p && p->value() == token_;
}

void Security::setPassword(const String &password) {
  if (password.isEmpty()) {
    digestHash_ = otaHash_ = "";
  } else {
    // HA1 of HTTP digest auth: MD5(user:realm:password). The server can check
    // a digest response with this alone, so the password itself is never stored.
    digestHash_ = generateDigestHash(kUser, password.c_str(), kRealm);
    MD5Builder md5;
    md5.begin();
    md5.add(password);
    md5.calculate();
    otaHash_ = md5.toString();
  }
  // A new token logs out existing WebSocket sessions on their next reconnect.
  newToken();
  save();
  ESP_LOGW(TAG, "Web password %s", passwordSet() ? "changed" : "removed");
  if (onChange_) onChange_();
}

void Security::erase() {
  Preferences prefs;
  if (prefs.begin(kPrefsNamespace, false)) {
    prefs.clear();
    prefs.end();
  }
  digestHash_ = otaHash_ = "";
}

void Security::newToken() {
  uint8_t raw[16];
  esp_fill_random(raw, sizeof(raw));
  for (size_t i = 0; i < sizeof(raw); i++) snprintf(token_ + i * 2, 3, "%02x", raw[i]);
}

void Security::save() {
  led::FlashGuard guard;  // LED strips pause during the flash write
  Preferences prefs;
  if (!prefs.begin(kPrefsNamespace, false)) return;
  prefs.putString("digest", digestHash_);
  prefs.putString("ota", otaHash_);
  prefs.end();
}

}  // namespace sb
