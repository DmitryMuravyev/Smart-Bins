#include "SendGate.h"

#include "ApiUtil.h"

namespace sb {

SendGate &sendGate() {
  static SendGate gate;
  return gate;
}

SendGate::Slot SendGate::take() {
  active_++;
  return std::make_shared<Token>(*this);
}

size_t SendGate::waiting() {
  std::lock_guard<std::mutex> lock(mutex_);
  return queue_.size();
}

void SendGate::run(AsyncWebServerRequest *req, Start start) {
  Slot slot;
  {
    // The check and the take happen under the lock: loop() starts queued
    // requests from another task, and the two must not both take the last slot.
    std::lock_guard<std::mutex> lock(mutex_);
    // First come, first served: a free slot goes to the queue before a newcomer.
    if (active_ < kMaxActive && queue_.empty()) {
      slot = take();
    } else if (queue_.size() < kMaxWaiting) {
      queue_.push_back({req->pause(), std::move(start)});
      return;
    }
  }
  if (!slot) return sendError(req, 503, "busy, try again");
  start(req, std::move(slot));
}

void SendGate::loop() {
  for (;;) {
    Pending next;
    Slot slot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.empty() || active_ >= kMaxActive) return;
      next = std::move(queue_.front());
      queue_.pop_front();
      slot = take();
    }
    // The browser may have given up while it waited: then nothing is sent,
    // and dropping `slot` frees it for the next one.
    if (std::shared_ptr<AsyncWebServerRequest> req = next.req.lock()) next.start(req.get(), std::move(slot));
  }
}

}  // namespace sb
