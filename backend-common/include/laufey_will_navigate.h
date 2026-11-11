// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Will-navigate (API 56): the embedder sees a window's top-level navigation
// before the navigation guard (laufey_navigation_guard.h) decides it.
//
// With a handler set (set_will_navigate_handler), each top-level navigation a
// page starts is reported (Ask) and waits for answer_will_navigate:
//   DEFAULT  the guard decides, as it would have with no handler;
//   ALLOW    it loads in the window, whatever the guard would say;
//   REFUSE   it is cancelled, and nothing opens in the browser either.
// The reply runs exactly once, on whichever thread decides: the answer, the
// deadline (kWillNavigateTimeout: DEFAULT, so an embedder that never answers
// leaves the guard in charge), the window closing (ForgetWindow: REFUSE) or
// the handler being cleared (DEFAULT). A backend whose engine must be answered
// on its UI thread posts the decision there itself.
//
// Not reported (WillNavigateReported and the tokens below): about:blank and
// about:srcdoc, the embedder's own navigate() (ExpectEmbedderNavigation, set
// where the backend records the window's URL for the guard), and a navigation
// a backend that cannot hold one (CEF, WebView2) cancelled and starts again
// once decided (ExpectReissue). Tokens are matched on the URL
// (NavigationMatchKey) and expire after kWillNavigateTokenLifetime, so a
// token for a load that never happened can't let a later one through.
//
// A backend that cannot hold a navigation (CEF, WebView2) never holds a
// non-GET one either (WillNavigateHoldsMethod): starting it again would send
// its body on the window's authority, not on that of the frame that really
// submitted it (a cross-origin sub-frame's `target=_top` form would arrive as
// same-origin), and WebView2 can only start a GET. Such a navigation is not
// reported; the guard decides it at once, as with no handler. A re-issue token
// is bound to the load the backend starts itself: the window's next
// navigation start spends it whether or not it matches, and it lets through
// only a navigation the engine says the backend started
// (`started_by_backend`), so a page's own navigation to the same URL is
// reported and guarded as usual.
//
// Header-only.

#ifndef LAUFEY_WILL_NAVIGATE_H_
#define LAUFEY_WILL_NAVIGATE_H_

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "laufey.h"

namespace laufey_common {

// How long a reported navigation waits for the embedder's answer.
inline constexpr std::chrono::milliseconds kWillNavigateTimeout{10000};
// How long an embedder-navigation or re-issue token stays valid.
inline constexpr std::chrono::milliseconds kWillNavigateTokenLifetime{10000};

// Whether a top-level navigation to `url` is reported at all: not
// about:blank / about:srcdoc (with any query or fragment).
inline bool WillNavigateReported(const std::string& url) {
  std::string head = url.substr(0, url.find_first_of("?#"));
  std::string lower;
  lower.reserve(head.size());
  for (char c : head)
    lower.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a')
                                           : c);
  return !url.empty() && lower != "about:blank" && lower != "about:srcdoc";
}

// The form two spellings of one URL share, as engines normalize what they
// were asked to load: the scheme and host lowercased, an empty path after
// the authority read as "/", and the fragment dropped.
inline std::string NavigationMatchKey(const std::string& url) {
  std::string u = url.substr(0, url.find('#'));
  size_t colon = u.find(':');
  if (colon == std::string::npos)
    return u;
  std::string out;
  out.reserve(u.size() + 1);
  for (size_t i = 0; i < colon; ++i) {
    char c = u[i];
    out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a')
                                         : c);
  }
  out.push_back(':');
  if (u.compare(colon + 1, 2, "//") != 0)
    return out + u.substr(colon + 1);
  size_t host_start = colon + 3;
  size_t host_end = u.find_first_of("/?", host_start);
  if (host_end == std::string::npos)
    host_end = u.size();
  out += "//";
  for (size_t i = host_start; i < host_end; ++i) {
    char c = u[i];
    out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a')
                                         : c);
  }
  if (host_end == u.size() || u[host_end] == '?')
    out.push_back('/');
  out += u.substr(host_end);
  return out;
}

// Whether a backend that cancels and restarts a navigation (CEF, WebView2)
// may hold one with this HTTP method for will-navigate: GET only (an empty
// method is GET). A non-GET one goes to the guard at once, unreported.
inline bool WillNavigateHoldsMethod(const std::string& method) {
  if (method.empty())
    return true;
  if (method.size() != 3)
    return false;
  auto up = [](char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  };
  return up(method[0]) == 'G' && up(method[1]) == 'E' && up(method[2]) == 'T';
}

// Whether `header_names` holds `lower_name` (given lowercase), in any case.
inline bool HeaderNamesContain(const std::vector<std::string>& header_names,
                               const std::string& lower_name) {
  for (const std::string& name : header_names) {
    if (name.size() != lower_name.size())
      continue;
    bool same = true;
    for (size_t i = 0; i < name.size() && same; ++i) {
      char c = name[i];
      if (c >= 'A' && c <= 'Z')
        c = static_cast<char>(c - 'A' + 'a');
      same = c == lower_name[i];
    }
    if (same)
      return true;
  }
  return false;
}

// Whether a navigation's request header names say it is not a GET, for an
// engine that reports headers but no method (WebView2's NavigationStarting):
// a form submission carries `Content-Type` and the engine adds `Origin` to a
// non-GET navigation; a GET carries neither. Errs toward non-GET (a GET read
// as non-GET is merely left to the guard).
inline bool NavigationHeadersMarkNonGet(
    const std::vector<std::string>& header_names) {
  return HeaderNamesContain(header_names, "content-type") ||
         HeaderNamesContain(header_names, "origin");
}

// A decision the embedder may give; anything else reads as DEFAULT.
inline uint32_t NormalizeWillNavigateDecision(uint32_t decision) {
  return decision == LAUFEY_NAVIGATION_ALLOW ||
                 decision == LAUFEY_NAVIGATION_REFUSE
             ? decision
             : LAUFEY_NAVIGATION_DEFAULT;
}

enum class WillNavigateStart {
  // No handler: nothing was reported; the guard decides now.
  kNoHandler,
  // Reported: the reply runs once the decision is made.
  kAsked,
};

class WillNavigateRequests {
 public:
  using Reply = std::function<void(uint32_t decision)>;
  using Clock = std::chrono::steady_clock;

  static WillNavigateRequests& Instance() {
    static auto* instance = new WillNavigateRequests();
    return *instance;
  }

  // A null handler clears it; every navigation still waiting goes to the
  // guard (DEFAULT). Any thread.
  void SetHandler(laufey_will_navigate_fn handler, void* user_data) {
    std::vector<Reply> released;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      handler_ = handler;
      user_data_ = user_data;
      if (!handler) {
        for (auto& [id, pending] : pending_)
          released.push_back(std::move(pending.reply));
        pending_.clear();
      }
    }
    for (Reply& reply : released)
      reply(LAUFEY_NAVIGATION_DEFAULT);
  }

  bool HandlerSet() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return handler_ != nullptr;
  }

  // Reports a top-level navigation of `window_id` to `url` (the caller has
  // checked WillNavigateReported and the tokens). With no handler nothing is
  // reported and `reply` is not called. The handler runs on this thread (the
  // UI thread), outside the lock. `timeout` <= 0 never expires (tests).
  WillNavigateStart Ask(
      uint32_t window_id, const std::string& url, const std::string& initiator,
      uint32_t kind, Reply reply,
      std::chrono::milliseconds timeout = kWillNavigateTimeout) {
    laufey_will_navigate_fn handler = nullptr;
    void* user_data = nullptr;
    uint64_t id = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!handler_)
        return WillNavigateStart::kNoHandler;
      id = ++last_id_;
      Pending pending{window_id, std::move(reply), {}};
      if (timeout.count() > 0) {
        pending.deadline = Clock::now() + timeout;
        StartTimerLocked();
      }
      pending_[id] = std::move(pending);
      handler = handler_;
      user_data = user_data_;
    }
    timer_cv_.notify_all();
    handler(user_data, window_id, id, url.c_str(),
            initiator.empty() ? nullptr : initiator.c_str(), kind);
    return WillNavigateStart::kAsked;
  }

  // Answers navigation `id`; false when it is not waiting. Any thread.
  bool Answer(uint64_t id, uint32_t decision) {
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
      reply(NormalizeWillNavigateDecision(decision));
    return true;
  }

  // The window closed: its navigations are refused, and its tokens dropped.
  void ForgetWindow(uint32_t window_id) {
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
      embedder_.erase(window_id);
      reissue_.erase(window_id);
    }
    for (Reply& reply : refused) {
      if (reply)
        reply(LAUFEY_NAVIGATION_REFUSE);
    }
  }

  // Every navigation whose deadline is at or before `now` goes to the guard
  // (DEFAULT). Returns how many. The timer thread calls it; tests too.
  size_t ExpireDue(Clock::time_point now) {
    std::vector<Reply> expired;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = pending_.begin(); it != pending_.end();) {
        if (it->second.deadline != Clock::time_point() &&
            it->second.deadline <= now) {
          expired.push_back(std::move(it->second.reply));
          it = pending_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (Reply& reply : expired) {
      if (reply)
        reply(LAUFEY_NAVIGATION_DEFAULT);
    }
    return expired.size();
  }

  size_t PendingCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
  }

  // The embedder is navigating `window_id` to `url` itself (navigate()):
  // that load is not reported.
  void ExpectEmbedderNavigation(uint32_t window_id, const std::string& url,
                                Clock::time_point now = Clock::now()) {
    std::lock_guard<std::mutex> lock(mutex_);
    embedder_[window_id] =
        Token{NavigationMatchKey(url), now + kWillNavigateTokenLifetime};
  }

  // Whether the navigation of `window_id` to `url` is the embedder's own
  // (consumes the token when it matches).
  bool TakeEmbedderNavigation(uint32_t window_id, const std::string& url,
                              Clock::time_point now = Clock::now()) {
    std::lock_guard<std::mutex> lock(mutex_);
    return TakeLocked(&embedder_, window_id, url, now);
  }

  // A backend that cancelled a navigation to decide it starts it again:
  // that load is already decided (neither reported nor guarded). Call it
  // right before starting the load, on the UI thread.
  void ExpectReissue(uint32_t window_id, const std::string& url,
                     Clock::time_point now = Clock::now()) {
    std::lock_guard<std::mutex> lock(mutex_);
    reissue_[window_id] =
        Token{NavigationMatchKey(url), now + kWillNavigateTokenLifetime};
  }

  // Whether this navigation start of `window_id` is the backend's own
  // re-issue. Call it for every top-level navigation start of the window,
  // first: the window's next start after ExpectReissue spends the token
  // whether or not it matches, so a token never waits for a later load to
  // the same URL. It matches only when the URL is the re-issued one and the
  // engine says the backend started this load (`started_by_backend`: CEF's
  // explicit transition, WebView2's API navigation), never a page's own.
  bool TakeReissue(uint32_t window_id, const std::string& url,
                   bool started_by_backend,
                   Clock::time_point now = Clock::now()) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = reissue_.find(window_id);
    if (it == reissue_.end())
      return false;
    Token token = it->second;
    reissue_.erase(it);
    return started_by_backend && token.expires >= now &&
           token.key == NavigationMatchKey(url);
  }

 private:
  struct Pending {
    uint32_t window_id = 0;
    Reply reply;
    Clock::time_point deadline;
  };
  struct Token {
    std::string key;
    Clock::time_point expires;
  };

  WillNavigateRequests() = default;

  static bool TakeLocked(std::map<uint32_t, Token>* tokens, uint32_t window_id,
                         const std::string& url, Clock::time_point now) {
    auto it = tokens->find(window_id);
    if (it == tokens->end())
      return false;
    if (it->second.expires < now) {
      tokens->erase(it);
      return false;
    }
    if (it->second.key != NavigationMatchKey(url))
      return false;
    tokens->erase(it);
    return true;
  }

  // One thread for every deadline, started on the first navigation that has
  // one. It sleeps until the earliest deadline (or a new navigation) and
  // hands what expired to the guard. Never joined: the instance lives for
  // the process.
  void StartTimerLocked() {
    if (timer_started_)
      return;
    timer_started_ = true;
    std::thread([this] {
      std::unique_lock<std::mutex> lock(mutex_);
      for (;;) {
        Clock::time_point next = (Clock::time_point::max)();
        for (auto& [id, pending] : pending_) {
          if (pending.deadline != Clock::time_point() &&
              pending.deadline < next)
            next = pending.deadline;
        }
        if (next == (Clock::time_point::max)()) {
          timer_cv_.wait(lock);
          continue;
        }
        if (timer_cv_.wait_until(lock, next) == std::cv_status::timeout) {
          lock.unlock();
          ExpireDue(Clock::now());
          lock.lock();
        }
      }
    }).detach();
  }

  mutable std::mutex mutex_;
  std::condition_variable timer_cv_;
  bool timer_started_ = false;
  laufey_will_navigate_fn handler_ = nullptr;
  void* user_data_ = nullptr;
  uint64_t last_id_ = 0;
  std::map<uint64_t, Pending> pending_;
  std::map<uint32_t, Token> embedder_;
  std::map<uint32_t, Token> reissue_;
};

}  // namespace laufey_common

#endif  // LAUFEY_WILL_NAVIGATE_H_
