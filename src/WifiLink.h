#pragma once

// Wi-Fi station with a fallback setup portal, mDNS and over-the-air updates.
//
// - With saved credentials the device joins the home network as <name>.local.
// - Without credentials, or when the network can't be reached for a while, it
//   opens an access point "SmartBins-XXXX" with a captive setup page.
// - Credentials live in NVS (not in config.json, so backups never contain them).

#include <Arduino.h>

class AsyncWebServer;

namespace sb {

class WifiLink {
 public:
  enum class State : uint8_t { Idle, Connecting, Connected, Portal };

  // Registers the setup routes on `server`; call before server.begin().
  void begin(const char *hostName, AsyncWebServer &server);
  void loop();

  State state() const { return state_; }
  bool portalActive() const { return portalActive_; }
  String ipAddress() const;

  // Clears saved credentials and opens the setup portal.
  void forgetCredentials();

 private:
  void registerRoutes(AsyncWebServer &server);
  void startConnecting();
  void startPortal();
  void stopPortal();
  void onConnected();

  char host_[33] = "smartbins";
  char apName_[24] = "SmartBins";
  String ssid_;
  String pass_;
  State state_ = State::Idle;
  bool portalActive_ = false;
  bool servicesStarted_ = false;
  uint32_t stateSince_ = 0;
  uint32_t lastRetry_ = 0;
  uint32_t portalCloseAt_ = 0;

  // Set from web handlers (AsyncTCP task), applied in loop().
  volatile bool pendingConnect_ = false;
  String pendingSsid_;
  String pendingPass_;
};

}  // namespace sb
