#include "WifiLink.h"

#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_ota_ops.h>

#include "Health.h"
#include "led/RmtStrip.h"
#include "web/SetupPage.h"

namespace sb {

namespace {
// The 404 page for addresses outside the API (see onNotFound below). Plain
// HTML with its own colors for a light or dark screen, no scripts.
const char kNotFoundPage[] = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Bins: not found</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;padding:40px 16px;background:#f6f7f9;color:#1d2330;text-align:center}
@media(prefers-color-scheme:dark){body{background:#14171c;color:#e6e8ec}a{color:#7aa7ff}}
h1{font-size:22px;margin:0 0 16px}p{margin:6px 0;color:#7a8190}a{display:inline-block;margin-top:20px;font-weight:600}
</style></head><body>
<h1>404</h1>
<p>This page doesn't exist.</p>
<p>Esta página no existe.</p>
<p>Такой страницы нет.</p>
<a href="/">Smart Bins</a>
</body></html>)HTML";
}  // namespace

namespace {
const char *TAG = "net";
const char *kPrefsNamespace = "wifi";

constexpr uint32_t kConnectTimeoutMs = 20000;    // first attempt before the portal opens
constexpr uint32_t kLostLinkPortalMs = 60000;    // link down this long -> open the portal
constexpr uint32_t kPortalRetryMs = 60000;       // retry the saved network while the portal is up
constexpr uint32_t kPortalLingerMs = 60000;      // keep the portal after connecting so the phone sees the result

DNSServer dns;
}  // namespace

void WifiLink::begin(const char *hostName, const config::NetworkConfig &network, AsyncWebServer &server) {
  network_ = network;
  strncpy(host_, hostName, sizeof(host_) - 1);
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(apName_, sizeof(apName_), "SmartBins-%02X%02X", mac[4], mac[5]);

  WiFi.persistent(false);  // credentials are kept in our own NVS namespace
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(host_);
  WiFi.mode(WIFI_STA);

  Preferences prefs;
  prefs.begin(kPrefsNamespace, false);  // read-write: creates the namespace on first boot instead of logging an error
  ssid_ = prefs.getString("ssid", "");
  pass_ = prefs.getString("pass", "");
  prefs.end();

  registerRoutes(server);

  if (ssid_.isEmpty()) {
    ESP_LOGI(TAG, "No Wi-Fi credentials saved");
    startPortal();
  } else {
    startConnecting();
  }
}

String WifiLink::ipAddress() const {
  return state_ == State::Connected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
}

void WifiLink::startConnecting() {
  ESP_LOGI(TAG, "Connecting to \"%s\"", ssid_.c_str());
  if (network_.staticIp) {
    // A fixed address has to be set before WiFi.begin(), otherwise DHCP runs.
    // With no DNS server given, the gateway is used (most home routers are one).
    const auto &n = network_;
    const bool dnsSet = n.dns[0] | n.dns[1] | n.dns[2] | n.dns[3];
    const IPAddress ip(n.ip[0], n.ip[1], n.ip[2], n.ip[3]);
    const IPAddress gateway(n.gateway[0], n.gateway[1], n.gateway[2], n.gateway[3]);
    const IPAddress subnet(n.subnet[0], n.subnet[1], n.subnet[2], n.subnet[3]);
    const IPAddress dns = dnsSet ? IPAddress(n.dns[0], n.dns[1], n.dns[2], n.dns[3]) : gateway;
    if (WiFi.config(ip, gateway, subnet, dns)) {
      ESP_LOGI(TAG, "Fixed address %s", ip.toString().c_str());
    } else {
      ESP_LOGE(TAG, "Fixed address %s rejected, using DHCP", ip.toString().c_str());
    }
  }
  WiFi.begin(ssid_.c_str(), pass_.c_str());
  state_ = State::Connecting;
  stateSince_ = lastRetry_ = millis();
}

void WifiLink::startPortal() {
  if (!portalActive_) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(apName_);
    dns.setErrorReplyCode(DNSReplyCode::NoError);
    dns.start(53, "*", WiFi.softAPIP());
    portalActive_ = true;
    ESP_LOGW(TAG, "Setup portal open: join Wi-Fi \"%s\" and browse to http://%s/", apName_,
             WiFi.softAPIP().toString().c_str());
  }
  portalCloseAt_ = 0;
  if (state_ != State::Connected) {
    state_ = State::Portal;
    stateSince_ = lastRetry_ = millis();
  }
}

void WifiLink::stopPortal() {
  if (!portalActive_) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalActive_ = false;
  portalCloseAt_ = 0;
  ESP_LOGI(TAG, "Setup portal closed");
}

void WifiLink::onConnected() {
  state_ = State::Connected;
  stateSince_ = millis();
  ESP_LOGI(TAG, "Connected to \"%s\", IP %s, RSSI %d dBm", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
           WiFi.RSSI());
  if (portalActive_) portalCloseAt_ = millis() + kPortalLingerMs;

  if (!servicesStarted_) {
    // ArduinoOTA.begin() also starts mDNS under the same host name.
    ArduinoOTA.setHostname(host_);
    if (otaHash_.length()) ArduinoOTA.setPasswordHash(otaHash_.c_str());
    // An upload over ArduinoOTA writes the flash for its whole length: the
    // strips pause from start to end (see FlashGuard). All three callbacks
    // run on the loop task, inside ArduinoOTA.handle(), so the lock and its
    // unlock are on the same task as the gate requires. onError also comes
    // without an onStart (a wrong password), hence the flag.
    static bool otaPaused = false;
    ArduinoOTA.onStart([]() {
      if (!otaPaused) led::FlashGuard::lock();
      otaPaused = true;
      health::pauseLoopWatch(true);  // the upload runs inside loop() (see Health.h)
      ESP_LOGW(TAG, "OTA update started");
    });
    auto resume = []() {
      if (otaPaused) led::FlashGuard::unlock();
      otaPaused = false;
      health::pauseLoopWatch(false);
    };
    ArduinoOTA.onEnd([resume]() {
      resume();
      ESP_LOGW(TAG, "OTA update finished, restarting");
    });
    ArduinoOTA.onError([resume](ota_error_t e) {
      resume();
      ESP_LOGE(TAG, "OTA error %u", (unsigned)e);
    });
    ArduinoOTA.begin();
    // 4 MB boards have a single app slot (partitions/4mb.csv), so a Wi-Fi
    // upload would have nowhere to go and fail; ArduinoOTA still runs because
    // it is also what announces the board over mDNS.
    if (!esp_ota_get_next_update_partition(nullptr)) ESP_LOGW(TAG, "No second app slot: update this board over USB");
    MDNS.addService("http", "tcp", 80);
    // Network time (UTC), for the created/updated times of inventory records.
    // Runs in the background and keeps the clock in step; until the first
    // answer the clock reads 1970 and records get no time (see main.cpp).
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    servicesStarted_ = true;
  }
  ESP_LOGI(TAG, "Open http://%s.local/ or http://%s/", host_, WiFi.localIP().toString().c_str());
}

void WifiLink::setOtaPasswordHash(const String &md5) {
  otaHash_ = md5;
  // ArduinoOTA checks the stored hash on each upload, so a change made after
  // begin() applies to the next upload. An empty hash turns the check off.
  if (servicesStarted_) ArduinoOTA.setPasswordHash(otaHash_.c_str());
}

void WifiLink::forgetCredentials() {
  {
    led::FlashGuard guard;  // LED strips pause during the flash write
    Preferences prefs;
    prefs.begin(kPrefsNamespace, false);
    prefs.clear();
    prefs.end();
  }
  ssid_ = pass_ = "";
  WiFi.disconnect(false, true);
  state_ = State::Idle;
  ESP_LOGW(TAG, "Wi-Fi credentials erased");
  startPortal();
}

void WifiLink::loop() {
  const uint32_t now = millis();

  // New credentials arrive from the web server's task (POST /api/wifi). They
  // are applied here, on the loop task, because the HTTP response has to go
  // out before the radio leaves the current network, and Wi-Fi and NVS calls
  // should not run inside the async web server's callback.
  if (pendingConnect_) {
    pendingConnect_ = false;
    ssid_ = pendingSsid_;
    pass_ = pendingPass_;
    {
      led::FlashGuard guard;  // LED strips pause during the flash write
      Preferences prefs;
      prefs.begin(kPrefsNamespace, false);
      prefs.putString("ssid", ssid_);
      prefs.putString("pass", pass_);
      prefs.end();
    }
    WiFi.disconnect(false, false);
    startConnecting();
  }

  if (portalActive_) dns.processNextRequest();
  if (servicesStarted_) ArduinoOTA.handle();

  const bool linked = WiFi.status() == WL_CONNECTED;
  switch (state_) {
    case State::Connecting:
      if (linked) {
        onConnected();
      } else if (now - stateSince_ > kConnectTimeoutMs) {
        ESP_LOGW(TAG, "Could not join \"%s\"", ssid_.c_str());
        startPortal();  // no-op if already open; switches the state to Portal
      }
      break;

    case State::Portal:
      if (linked) {
        onConnected();
      } else if (!ssid_.isEmpty() && now - lastRetry_ > kPortalRetryMs && WiFi.softAPgetStationNum() == 0) {
        // Retry only while nobody is using the portal: a retry briefly disturbs the access point.
        lastRetry_ = now;
        ESP_LOGI(TAG, "Retrying \"%s\"", ssid_.c_str());
        WiFi.begin(ssid_.c_str(), pass_.c_str());
      }
      break;

    // Short drops are left to the driver's auto-reconnect. Only a long outage
    // opens the portal, so a router reboot doesn't expose a setup network.
    case State::Connected:
      if (linked) {
        stateSince_ = now;  // last time the link was seen up
        if (portalCloseAt_ && (int32_t)(now - portalCloseAt_) >= 0 && WiFi.softAPgetStationNum() == 0) stopPortal();
      } else if (now - stateSince_ > kLostLinkPortalMs) {
        ESP_LOGW(TAG, "Wi-Fi link lost for %lus", (unsigned long)(kLostLinkPortalMs / 1000));
        state_ = State::Portal;
        startPortal();
      }
      break;

    case State::Idle:
      break;
  }
}

void WifiLink::registerRoutes(AsyncWebServer &server) {
  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/html; charset=utf-8", web::kSetupPage);
  });

  server.on("/api/wifi/status", HTTP_GET, [this](AsyncWebServerRequest *req) {
    JsonDocument doc;
    static const char *const kStates[] = {"idle", "connecting", "connected", "portal"};
    doc["state"] = kStates[static_cast<int>(state_)];
    doc["ssid"] = ssid_;
    doc["ip"] = state_ == State::Connected ? WiFi.localIP().toString() : String();
    doc["host"] = String(host_) + ".local";
    doc["rssi"] = state_ == State::Connected ? WiFi.RSSI() : 0;
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  // Scanning takes a few seconds, too long to block a request. The first call
  // starts an asynchronous scan and answers {"scanning": true}; the setup page
  // polls until the results are in. Results are deleted once delivered, so the
  // next call starts a fresh scan.
  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
    JsonDocument doc;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) {
      WiFi.scanNetworks(true);
      n = WIFI_SCAN_RUNNING;
    }
    doc["scanning"] = n == WIFI_SCAN_RUNNING;
    JsonArray list = doc["networks"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
      const String ssid = WiFi.SSID(i);
      if (ssid.isEmpty()) continue;
      bool duplicate = false;  // same SSID from several access points: keep the strongest (listed first)
      for (JsonVariantConst seen : list) duplicate |= ssid == seen["ssid"].as<const char *>();
      if (duplicate) continue;
      JsonObject net = list.add<JsonObject>();
      net["ssid"] = ssid;
      net["rssi"] = WiFi.RSSI(i);
      net["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    if (n >= 0) WiFi.scanDelete();
    String out;
    serializeJson(doc, out);
    req->send(200, "application/json", out);
  });

  server.on("/api/wifi", HTTP_POST, [this](AsyncWebServerRequest *req) {
    if (!req->hasParam("ssid", true)) {
      req->send(400, "application/json", "{\"error\":\"ssid missing\"}");
      return;
    }
    const String ssid = req->getParam("ssid", true)->value();
    const String pass = req->hasParam("pass", true) ? req->getParam("pass", true)->value() : String();
    if (ssid.isEmpty() || ssid.length() > 32 || pass.length() > 63) {
      req->send(400, "application/json", "{\"error\":\"invalid ssid or password length\"}");
      return;
    }
    pendingSsid_ = ssid;
    pendingPass_ = pass;
    pendingConnect_ = true;
    req->send(200, "application/json", "{\"ok\":true}");
  });

  // Captive portal: phones probe well-known URLs on other hosts; send them to the setup page.
  server.onNotFound([this](AsyncWebServerRequest *req) {
    if (portalActive_ && req->host() != WiFi.softAPIP().toString() && req->host() != String(host_) + ".local") {
      req->redirect(("http://" + WiFi.softAPIP().toString() + "/wifi").c_str());
      return;
    }
    // The API answers in JSON, like its other errors, so a script or the
    // page gets a message it can show.
    if (req->url().startsWith("/api/")) {
      req->send(404, "application/json", "{\"error\":\"no such API endpoint\"}");
      return;
    }
    // Anything else (a mistyped address, an old bookmark): a short page in
    // the three UI languages with a way back, instead of a bare "Not found".
    req->send(404, "text/html; charset=utf-8", kNotFoundPage);
  });
}

}  // namespace sb
