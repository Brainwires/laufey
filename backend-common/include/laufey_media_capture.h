// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Microphone and camera requests (API 53): who may capture.
//
// A page's getUserMedia reaches each engine's permission hook (WKWebView's
// requestMediaCapturePermissionForOrigin, CEF's
// OnRequestMediaAccessPermission, WebView2's PermissionRequested, WebKitGTK's
// permission-request). The backend hands it to RequestMediaCapture, which
// decides it exactly once, refusing by default:
//
//   1. refused: nothing asked for, anything but the microphone and camera
//      (screen capture), an opaque origin, or an asking document whose
//      origin is not the window's top-level document's (a sub-frame of
//      another origin never gets the devices, whatever anyone says);
//   2. allowed: the launch file's "mediaCapture" grants the origin every kind
//      it asks for (exact origins only; docs/launch-config.md);
//   3. asked: the runtime's handler (set_media_capture_handler), which answers
//      through answer_media_capture; with no handler, refused.
//
// A request that is asked waits until the runtime answers, its window closes
// (close_window: refused), its window's top-level document is replaced (the
// main frame commits a navigation to another document: refused, so a late
// answer never reaches whatever page is there now; ForgetDocument), or the
// handler is cleared (refused). The backend's reply runs exactly once, on
// whichever thread decides; a backend whose engine must be answered on its UI
// thread posts the answer there itself, and there re-checks that the window's
// top-level document is still of the origin the request was made for
// (MediaCaptureGrantStands): a grant that no longer is becomes a refusal.
//
// Origins are compared in their HTML serialization (laufey_bridge_origin.h).
// Header-only.

#ifndef LAUFEY_MEDIA_CAPTURE_H_
#define LAUFEY_MEDIA_CAPTURE_H_

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "laufey.h"
#include "laufey_bridge_origin.h"
#include "laufey_launch_config.h"

namespace laufey_common {

// Every kind a request may be granted.
inline constexpr uint32_t kMediaCaptureKnownKinds =
    LAUFEY_MEDIA_CAPTURE_MICROPHONE | LAUFEY_MEDIA_CAPTURE_CAMERA;
// What an engine reports for a kind laufey never grants (screen, window or
// tab capture, system audio): a request with this bit is refused.
inline constexpr uint32_t kMediaCaptureOther = 1u << 31;

// The origins the launch file grants each kind (serialized, exact).
struct MediaCapturePolicy {
  std::vector<std::string> microphone;
  std::vector<std::string> camera;
};

inline MediaCapturePolicy MediaCapturePolicyFrom(const LaunchConfig& config) {
  MediaCapturePolicy policy;
  if (config.has_media_capture) {
    policy.microphone = config.media_capture_microphone;
    policy.camera = config.media_capture_camera;
  }
  return policy;
}

// This process's policy, from its launch file (read once).
inline const MediaCapturePolicy& ProcessMediaCapturePolicy() {
  static const MediaCapturePolicy policy =
      MediaCapturePolicyFrom(ProcessLaunchConfig());
  return policy;
}

enum class MediaCaptureDecision { kAllow, kDeny, kAsk };

namespace media_capture_internal {

inline bool Contains(const std::vector<std::string>& list,
                     const std::string& origin) {
  for (const std::string& o : list) {
    if (o == origin)
      return true;
  }
  return false;
}

}  // namespace media_capture_internal

// At answer time, on the backend's UI thread: whether `allow` still holds for
// a request made while the window's top-level document was of
// `request_top_origin`, now that it is of `current_top_origin` (both
// serialized). A grant for a window whose top-level document has since moved
// to another origin (or to none) is a refusal. Pure.
inline bool MediaCaptureGrantStands(bool allow,
                                    const std::string& request_top_origin,
                                    const std::string& current_top_origin) {
  return allow && !request_top_origin.empty() &&
         request_top_origin != kOpaqueOrigin &&
         request_top_origin == current_top_origin;
}

// A sub-frame's document origin, for an engine whose permission event names
// the frame but neither its document nor its real origin (WebView2: a
// delegated request, `<iframe allow="microphone">`, carries the top-level
// document's origin, and an ICoreWebView2Frame has no URL). The backend feeds
// it the frame's own navigations; a request from the frame is decided with
// RequestingOrigin(). Unknown is refused (kOpaqueOrigin):
//   - before the frame commits a document whose start it saw (its initial
//     about:blank, a srcdoc frame), and after a navigation that never commits
//     (a 204 leaves the old document in place);
//   - once the frame has had a child frame: the engine raises a descendant's
//     request on the ancestor it knows, so the asking document can't be told.
// Not thread-safe: the backend's UI thread only. Pure otherwise.
class SubFrameOrigin {
 public:
  // A navigation started, or was redirected (same `navigation_id`), to a URL
  // of `origin` (serialized).
  void NavigationStarting(uint64_t navigation_id, const std::string& origin) {
    for (auto& [id, o] : starting_) {
      if (id == navigation_id) {
        o = origin;
        return;
      }
    }
    starting_.push_back({navigation_id, origin});
  }

  // Navigation `navigation_id` committed: its document is the frame's now.
  // One the frame never saw start leaves the origin unknown.
  void Committed(uint64_t navigation_id) {
    committed_ = kOpaqueOrigin;
    for (auto& [id, o] : starting_) {
      if (id == navigation_id)
        committed_ = o;
    }
    starting_.clear();
  }

  // The frame created a child frame (or the engine can't say whether it did).
  void ChildFrameCreated() {
    has_child_ = true;
  }

  std::string RequestingOrigin() const {
    return has_child_ || committed_.empty() ? std::string(kOpaqueOrigin)
                                            : committed_;
  }

 private:
  std::vector<std::pair<uint64_t, std::string>> starting_;
  std::string committed_;
  bool has_child_ = false;
};

// Steps 1-3 above for one request: `top_origin` is the window's top-level
// document's serialized origin, `requesting_origin` the asking document's,
// `kinds` LAUFEY_MEDIA_CAPTURE_* bits (plus kMediaCaptureOther for anything
// else), `handler_set` whether the runtime has a handler. Pure.
inline MediaCaptureDecision DecideMediaCapture(
    const MediaCapturePolicy& policy, const std::string& top_origin,
    const std::string& requesting_origin, uint32_t kinds, bool handler_set) {
  using media_capture_internal::Contains;
  if (kinds == 0 || (kinds & ~kMediaCaptureKnownKinds) != 0)
    return MediaCaptureDecision::kDeny;
  if (requesting_origin.empty() || requesting_origin == kOpaqueOrigin ||
      top_origin != requesting_origin)
    return MediaCaptureDecision::kDeny;
  bool granted = true;
  if (kinds & LAUFEY_MEDIA_CAPTURE_MICROPHONE)
    granted = granted && Contains(policy.microphone, requesting_origin);
  if (kinds & LAUFEY_MEDIA_CAPTURE_CAMERA)
    granted = granted && Contains(policy.camera, requesting_origin);
  if (granted)
    return MediaCaptureDecision::kAllow;
  return handler_set ? MediaCaptureDecision::kAsk : MediaCaptureDecision::kDeny;
}

// The requests waiting for the runtime, and its handler. One per process
// (Instance()); a separate instance only in tests.
class MediaCaptureRequests {
 public:
  using Reply = std::function<void(bool allow)>;

  static MediaCaptureRequests& Instance() {
    static auto* instance = new MediaCaptureRequests();
    return *instance;
  }

  // A null handler clears it and refuses every request still waiting. Any
  // thread.
  void SetHandler(laufey_media_capture_fn handler, void* user_data) {
    std::vector<Reply> refused;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      handler_ = handler;
      user_data_ = user_data;
      if (!handler) {
        for (auto& [id, pending] : pending_)
          refused.push_back(std::move(pending.reply));
        pending_.clear();
      }
    }
    for (Reply& reply : refused)
      reply(false);
  }

  bool HandlerSet() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handler_ != nullptr;
  }

  // Decides a request (see the top of the file) under `policy`; `reply` runs
  // exactly once: now for an allowed or refused request, later for an asked
  // one. Returns the decision. The handler is called on this thread (the UI
  // thread) and outside the lock.
  MediaCaptureDecision Request(const MediaCapturePolicy& policy,
                               uint32_t window_id,
                               const std::string& top_origin,
                               const std::string& requesting_origin,
                               uint32_t kinds, Reply reply) {
    laufey_media_capture_fn handler = nullptr;
    void* user_data = nullptr;
    uint64_t id = 0;
    MediaCaptureDecision decision;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      decision = DecideMediaCapture(policy, top_origin, requesting_origin,
                                    kinds, handler_ != nullptr);
      if (decision == MediaCaptureDecision::kAsk) {
        id = ++last_id_;
        pending_[id] = Pending{window_id, std::move(reply)};
        handler = handler_;
        user_data = user_data_;
      }
    }
    if (decision != MediaCaptureDecision::kAsk) {
      if (reply)
        reply(decision == MediaCaptureDecision::kAllow);
      return decision;
    }
    handler(window_id, id, requesting_origin.c_str(), kinds, user_data);
    return decision;
  }

  // The process's policy.
  MediaCaptureDecision Request(uint32_t window_id,
                               const std::string& top_origin,
                               const std::string& requesting_origin,
                               uint32_t kinds, Reply reply) {
    return Request(ProcessMediaCapturePolicy(), window_id, top_origin,
                   requesting_origin, kinds, std::move(reply));
  }

  // Answers request `id`; false when it is not waiting. Any thread.
  bool Answer(uint64_t id, bool allow) {
    Reply reply;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = pending_.find(id);
      if (it == pending_.end())
        return false;
      reply = std::move(it->second.reply);
      pending_.erase(it);
    }
    if (reply)
      reply(allow);
    return true;
  }

  // Refuses every request window `window_id` is waiting on (it closed).
  void ForgetWindow(uint32_t window_id) {
    RefuseWindow(window_id);
  }

  // Refuses every request window `window_id` is waiting on because its
  // top-level document was replaced: the backend calls it when the window's
  // main frame commits a navigation to another document (not for a
  // same-document one: a fragment, pushState). Requests the new document
  // makes afterwards are decided as usual. Any thread.
  void ForgetDocument(uint32_t window_id) {
    RefuseWindow(window_id);
  }

  size_t PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
  }

  MediaCaptureRequests() = default;
  MediaCaptureRequests(const MediaCaptureRequests&) = delete;
  MediaCaptureRequests& operator=(const MediaCaptureRequests&) = delete;

 private:
  struct Pending {
    uint32_t window_id = 0;
    Reply reply;
  };

  void RefuseWindow(uint32_t window_id) {
    std::vector<Reply> refused;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.window_id == window_id) {
          refused.push_back(std::move(it->second.reply));
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (Reply& reply : refused) {
      if (reply)
        reply(false);
    }
  }

  mutable std::mutex mutex_;
  laufey_media_capture_fn handler_ = nullptr;
  void* user_data_ = nullptr;
  uint64_t last_id_ = 0;
  std::map<uint64_t, Pending> pending_;
};

// Test hook: when LAUFEY_TEST_MOCK_CAPTURE_DEVICES is "1", a backend whose
// engine has stand-in capture devices turns them on (WebKit's mock devices,
// WebView2's --use-fake-device-for-media-stream browser argument), so the e2e
// battery can capture on a machine without a microphone or camera. The CEF
// host takes Chromium's switch on its command line instead.
inline bool MockCaptureDevicesForTests() {
  const char* v = std::getenv("LAUFEY_TEST_MOCK_CAPTURE_DEVICES");
  return v && std::string(v) == "1";
}

}  // namespace laufey_common

#endif  // LAUFEY_MEDIA_CAPTURE_H_
