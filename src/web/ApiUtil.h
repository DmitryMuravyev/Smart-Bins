#pragma once

// Small helpers shared by the JSON endpoints.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

namespace sb {

inline void sendJson(AsyncWebServerRequest *req, int code, const JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  req->send(code, "application/json", out);
}

// A Print that appends to a std::string, for building a JSON answer piece by
// piece (the parts list writes one part at a time).
//
// Why not AsyncResponseStream: it keeps its text in a ring buffer that grows
// by exactly the bytes that don't fit, and ArduinoJson writes a character at
// a time, so past the first 1460 bytes every character made a new ring
// buffer and copied everything written so far into it. A 50-part answer
// took about 20 s that way. std::string grows by doubling, so building the
// same answer costs a handful of copies.
class StringPrint : public Print {
public:
  explicit StringPrint(std::string &out) : out_(out) {}
  size_t write(uint8_t c) override {
    out_.push_back(static_cast<char>(c));
    return 1;
  }
  size_t write(const uint8_t *data, size_t len) override {
    out_.append(reinterpret_cast<const char *>(data), len);
    return len;
  }

private:
  std::string &out_;
};

// Sends text built in a std::string. The string is moved into the response
// and the server copies from it as the connection takes the data, so the
// answer exists in RAM only once (req->send(code, type, String) would copy
// it first).
inline void sendText(AsyncWebServerRequest *req, int code, const char *contentType, std::string &&body) {
  auto text = std::make_shared<std::string>(std::move(body));
  AsyncWebServerResponse *res =
      req->beginResponse(contentType, text->size(), [text](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
        // index is how much has been sent already; never past the end.
        const size_t n = index < text->size() ? std::min(maxLen, text->size() - index) : 0;
        memcpy(buf, text->data() + index, n);
        return n;
      });
  res->setCode(code);
  req->send(res);
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
