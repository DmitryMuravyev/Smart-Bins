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
#include <new>
#include <utility>
#include <vector>

namespace sb {

inline void sendJson(AsyncWebServerRequest *req, int code, const JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  req->send(code, "application/json", out);
}

// A Print that collects text in 1 KB blocks, for building a JSON answer
// piece by piece (the parts list writes one part at a time).
//
// Why not AsyncResponseStream: it keeps its text in a ring buffer that grows
// by exactly the bytes that don't fit, and ArduinoJson writes a character at
// a time, so past the first 1460 bytes every character made a new ring
// buffer and copied everything written so far into it. A 50-part answer
// took about 20 s that way.
//
// Why not one std::string: it grows by doubling, and each step briefly
// holds the old and the new buffer (a 30 KB answer passes through a 16 KB +
// 32 KB moment) and needs one contiguous block, which a fragmented heap may
// not have. Fixed 1 KB blocks need neither: the answer costs its own size
// plus a few bytes per block.
class TextChunks : public Print {
public:
  static constexpr size_t kChunk = 1024;

  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *data, size_t len) override {
    size_t done = 0;
    while (done < len) {
      if (chunks_.empty() || used_ == kChunk) {
        // Out of memory: remember it and drop the rest; the caller answers
        // with an error instead of a cut-off document (see failed()).
        std::unique_ptr<char[]> block(new (std::nothrow) char[kChunk]);
        if (!block) {
          failed_ = true;
          return done;
        }
        chunks_.push_back(std::move(block));
        used_ = 0;
      }
      const size_t n = std::min(len - done, kChunk - used_);
      memcpy(chunks_.back().get() + used_, data + done, n);
      used_ += n;
      done += n;
    }
    return done;
  }

  size_t size() const { return chunks_.empty() ? 0 : (chunks_.size() - 1) * kChunk + used_; }
  bool failed() const { return failed_; }

  // Copies up to maxLen bytes starting at `offset` into buf; returns how many.
  size_t read(size_t offset, uint8_t *buf, size_t maxLen) const {
    const size_t total = size();
    size_t copied = 0;
    while (copied < maxLen && offset < total) {
      const size_t block = offset / kChunk, at = offset % kChunk;
      const size_t inBlock = (block + 1 == chunks_.size() ? used_ : kChunk) - at;
      const size_t n = std::min(maxLen - copied, inBlock);
      memcpy(buf + copied, chunks_[block].get() + at, n);
      copied += n;
      offset += n;
    }
    return copied;
  }

private:
  std::vector<std::unique_ptr<char[]>> chunks_;
  size_t used_ = 0;  // bytes used in the last block
  bool failed_ = false;
};

// Sends text collected in TextChunks. The response keeps the blocks and the
// server copies from them as the connection takes the data, so the answer
// exists in RAM only once (req->send(code, type, String) would copy it into
// one big block first). If building the text ran out of memory, answers 503
// instead.
inline void sendChunks(AsyncWebServerRequest *req, int code, const char *contentType,
                       std::shared_ptr<TextChunks> text);

// Errors are always {"error": "..."} so the UI can show the message as it is.
inline void sendError(AsyncWebServerRequest *req, int code, const char *message) {
  JsonDocument doc;
  doc["error"] = message;
  sendJson(req, code, doc);
}

inline void sendChunks(AsyncWebServerRequest *req, int code, const char *contentType,
                       std::shared_ptr<TextChunks> text) {
  if (text->failed()) return sendError(req, 503, "out of memory, try again");
  AsyncWebServerResponse *res =
      req->beginResponse(contentType, text->size(), [text](uint8_t *buf, size_t maxLen, size_t index) -> size_t {
        // index is how much has been sent already.
        return text->read(index, buf, maxLen);
      });
  res->setCode(code);
  req->send(res);
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

// Reads a color given as 6 hex digits, with or without a leading '#'
// (an HTML color picker sends "#ff8800").
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
