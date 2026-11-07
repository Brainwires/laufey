// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The CEF backend's evaluations waiting for their result (execute_js with a
// callback). The host sends the script to one browser's main frame and the
// renderer answers with the eval id; the answer is a renderer's message, so
// it settles an evaluation only when it comes from the browser the script
// was sent to. A result from another browser, an id never issued, or a
// second answer settles nothing, and the evaluation keeps waiting for its
// own browser's answer. Header-only; `Payload` is what the backend keeps per
// evaluation (its callback).

#ifndef LAUFEY_PENDING_EVALS_H_
#define LAUFEY_PENDING_EVALS_H_

#include <cstdint>
#include <map>
#include <mutex>
#include <utility>

namespace laufey_common {

template <typename Payload>
class PendingEvals {
 public:
  // Records an evaluation sent to browser `browser_id` and returns its id
  // (never 0, never reused). Any thread.
  uint64_t Add(int browser_id, Payload payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = next_id_++;
    pending_[id] = Entry{browser_id, std::move(payload)};
    return id;
  }

  // Removes evaluation `id` and returns its payload when browser
  // `browser_id` is the one it was sent to; false (and the evaluation stays
  // pending) otherwise. Any thread.
  bool Take(uint64_t id, int browser_id, Payload* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pending_.find(id);
    if (it == pending_.end() || it->second.browser_id != browser_id)
      return false;
    if (out)
      *out = std::move(it->second.payload);
    pending_.erase(it);
    return true;
  }

  size_t size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
  }

 private:
  struct Entry {
    int browser_id = 0;
    Payload payload{};
  };
  std::mutex mutex_;
  uint64_t next_id_ = 1;
  std::map<uint64_t, Entry> pending_;
};

}  // namespace laufey_common

#endif  // LAUFEY_PENDING_EVALS_H_
