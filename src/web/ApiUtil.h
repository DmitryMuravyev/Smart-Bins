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

// Reads a whole-number query parameter. Returns false when it is missing, not a
// plain decimal number (strtol must consume the whole string) or out of range.
inline bool intParam(AsyncWebServerRequest *req, const char *name, long min, long max, long &out) {
  if (!req->hasParam(name)) return false;
  const String &v = req->getParam(name)->value();
  char *end = nullptr;
  long n = strtol(v.c_str(), &end, 10);
  if (v.isEmpty() || *end != '\0' || n < min || n > max) return false;
  out = n;
  return true;
}

// Reads a colour given as 6 hex digits, with or without a leading '#'
// (an HTML colour picker sends "#ff8800").
inline bool colorParam(AsyncWebServerRequest *req, const char *name, uint32_t &out) {
  if (!req->hasParam(name)) return false;
  String v = req->getParam(name)->value();
  if (v.startsWith("#")) v.remove(0, 1);
  char *end = nullptr;
  const unsigned long n = strtoul(v.c_str(), &end, 16);
  if (v.length() != 6 || *end != '\0') return false;
  out = static_cast<uint32_t>(n);
  return true;
}

}  // namespace sb
