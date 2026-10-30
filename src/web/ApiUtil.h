#pragma once

// Small helpers shared by the JSON endpoints.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <cstdlib>

namespace sb {

inline void sendJson(AsyncWebServerRequest *req, int code, const JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  req->send(code, "application/json", out);
}

// Errors are always {"error": "..."} so the UI can show the message as it is.
inline void sendError(AsyncWebServerRequest *req, int code, const char *message) {
  JsonDocument doc;
  doc["error"] = message;
  sendJson(req, code, doc);
}

// A request parameter from the query string or, for a POST, from a form-encoded
// body. Accepting both lets the UI use POST for changes while the plain URLs
// (handy for testing in a browser or from scripts) keep working.
inline const AsyncWebParameter *param(AsyncWebServerRequest *req, const char *name) {
  if (const AsyncWebParameter *p = req->getParam(name)) return p;
  return req->getParam(name, true);
}

inline bool hasParam(AsyncWebServerRequest *req, const char *name) { return param(req, name) != nullptr; }

// Reads a whole-number parameter. Returns false when it is missing, not a
// plain decimal number (strtol must consume the whole string) or out of range.
inline bool intParam(AsyncWebServerRequest *req, const char *name, long min, long max, long &out) {
  const AsyncWebParameter *p = param(req, name);
  if (!p) return false;
  const String &v = p->value();
  char *end = nullptr;
  long n = strtol(v.c_str(), &end, 10);
  if (v.isEmpty() || *end != '\0' || n < min || n > max) return false;
  out = n;
  return true;
}

// Reads a colour given as 6 hex digits, with or without a leading '#'
// (an HTML colour picker sends "#ff8800").
inline bool colorParam(AsyncWebServerRequest *req, const char *name, uint32_t &out) {
  const AsyncWebParameter *p = param(req, name);
  if (!p) return false;
  String v = p->value();
  if (v.startsWith("#")) v.remove(0, 1);
  char *end = nullptr;
  const unsigned long n = strtoul(v.c_str(), &end, 16);
  if (v.length() != 6 || *end != '\0') return false;
  out = static_cast<uint32_t>(n);
  return true;
}

}  // namespace sb
