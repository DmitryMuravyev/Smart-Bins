#pragma once

// Limits how many files and part lists the web server sends at the same time.
//
// Why: every response that is being sent holds about 8.6 KB of RAM: the
// server's 2.9 KB send buffer plus up to 5.7 KB of data handed to TCP and not
// yet acknowledged (both sizes are fixed by the framework's build settings).
// A browser opens up to six connections at once, so one phone loading the
// page, or a screen of part cards loading their thumbnails, could take some
// 50 KB of the 50-60 KB the board has free; two phones at once ran it out.
//
// With the gate, at most kMaxActive such responses run at a time. A request
// that finds the gate full is paused (the library's request continuation)
// and started from loop() as soon as a running one finishes. Files here are
// small (the UI is a few tens of KB gzipped, a thumbnail a few KB), so the
// wait is short, and the peak stays at about kMaxActive x 8.6 KB.
//
// Part lists (/api/parts) go through the same gate: the search runs when
// the gate lets the request through, and its answer is streamed part by
// part, so it costs about what a file does.
//
// Usage, in a request handler:
//
//   sendGate().run(req, [](AsyncWebServerRequest *req, SendGate::Slot slot) {
//     req->send(req->beginResponse(type, size, [slot](uint8_t *buf, size_t max, size_t index) { ... }));
//   });
//
// The response's filler keeps `slot` alive; the slot is given back when the
// response is deleted (sent, or the browser went away).

#include <ESPAsyncWebServer.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace sb {

class SendGate {
 public:
  static constexpr int kMaxActive = 2;
  // Requests waiting beyond this are answered 503 (the browser retries a
  // failed image when it is shown again; the UI files come first anyway).
  static constexpr size_t kMaxWaiting = 24;

  struct Token {
    explicit Token(SendGate &g) : gate(g) {}
    ~Token() { gate.active_--; }
    SendGate &gate;
  };
  using Slot = std::shared_ptr<Token>;
  using Start = std::function<void(AsyncWebServerRequest *, Slot)>;

  // Starts the response now if a slot is free, else queues the request.
  void run(AsyncWebServerRequest *req, Start start);
  // Starts queued requests as slots free up. Call from loop().
  void loop();

  int active() const { return active_; }
  size_t waiting();

 private:
  Slot take();

  struct Pending {
    AsyncWebServerRequestPtr req;
    Start start;
  };
  std::atomic<int> active_{0};
  std::mutex mutex_;
  std::deque<Pending> queue_;
};

SendGate &sendGate();

}  // namespace sb
