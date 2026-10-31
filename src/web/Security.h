#pragma once

// Optional password for the web UI and the API (off until one is set).
//
// - HTTP digest authentication, user "admin": the browser asks for the
//   password once and never sends it in clear text.
// - Only hashes are stored (NVS namespace "security"): the digest hash for the
//   web server and an MD5 hash for over-the-air updates, which then need the
//   same password (espota --auth).
// - The WebSocket can't answer a digest challenge in a browser, so it is
//   exempt from the HTTP check and instead needs a session token that only an
//   authenticated client can read from /api/session.
// - The Wi-Fi setup page and its API stay open on the setup access point, so a
//   device that lost its network can always be brought back. Everything else
//   on the access point still needs the password.

#include <Arduino.h>

class AsyncWebServer;
class AsyncWebServerRequest;

namespace sb {

class Security {
 public:
  // Loads the stored hashes, installs the check on every request and
  // registers /api/session and /api/security.
  void begin(AsyncWebServer &server);

  bool passwordSet() const { return digestHash_.length() > 0; }
  // MD5 of the password for ArduinoOTA, empty when no password is set.
  const String &otaHash() const { return otaHash_; }
  // True when the request may go ahead: no password, an exempt path, or a
  // valid digest response. The middleware runs this only after a request's
  // body has arrived, so handlers that store a body as it streams in (photo
  // uploads) call it themselves first.
  bool allows(AsyncWebServerRequest *req) const;
  // True when a WebSocket handshake carries the current session token (or no password is set).
  bool tokenValid(AsyncWebServerRequest *req) const;

  // Sets a new password; an empty string removes it.
  void setPassword(const String &password);
  // Removes the password (factory reset).
  void erase();

  // Called when the password changes, e.g. to update the OTA password.
  void onChange(void (*callback)()) { onChange_ = callback; }

 private:
  bool exempt(AsyncWebServerRequest *req) const;
  void newToken();
  void save();

  String digestHash_;
  String otaHash_;
  char token_[33] = "";
  void (*onChange_)() = nullptr;
};

}  // namespace sb
