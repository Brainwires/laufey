// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The navigation guard (API 48): what a window's top-level frame may load.
//
// A window shows the app. A top-level navigation to another origin (a link,
// `location = …`, a form, a server redirect, a meta refresh) never loads in
// the window: it is cancelled, and an http(s) destination is handed to the
// OS browser instead, through the same checked open-external path as the
// reserved `__laufeyOpenExternal` bridge call (IsAllowedExternalLinkUrl in
// laufey_external_links.h). `window.open()` and `target="_blank"` never
// create a web view either (the backends' new-window hooks).
//
// What a window's top-level frame may load:
//   - the origin of the URL the embedder last navigated it to (`navigate`):
//     the app's own origin, whatever it is (a custom scheme, a loopback
//     server, a dev server). The embedder's own navigation is never refused;
//   - the origins the embedder allows for every window
//     (set_allowed_navigation_origins, exact serialized origins);
//   - about:blank and about:srcdoc;
//   - for a window the embedder pointed at a file: URL, other file: URLs.
//
// Sub-frames are not the guard's business: what a page may frame is its
// Content Security Policy's (frame-src). Only top-level navigations are
// checked.
//
// Origins are compared in their HTML serialization (OriginOfUrl in
// laufey_bridge_origin.h).

#ifndef LAUFEY_NAVIGATION_GUARD_H_
#define LAUFEY_NAVIGATION_GUARD_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "laufey_bridge_origin.h"
#include "laufey_external_links.h"

namespace laufey_common {

// What a backend does with a top-level navigation.
enum class NavigationDecision {
  // Load it in the window.
  kAllow,
  // Cancel it and open the URL in the OS browser.
  kOpenInBrowser,
  // Cancel it (a destination the OS browser is not handed: file:, a custom
  // scheme, data:, javascript:, …).
  kBlock,
  // Cancel it without opening it: the window already handed more than
  // kMaxExternalOpensPerSecond navigations to the browser in the last
  // second (a page navigating away in a loop must not flood the browser).
  kThrottled,
};

// How many refused navigations a window may hand to the OS browser per
// second; the rest are cancelled and logged.
inline constexpr size_t kMaxExternalOpensPerSecond = 3;

// A sliding one-second window of the times a window's refused navigations
// went to the browser. `TryTake` records `now` and returns true while fewer
// than `limit` fall within the second before it.
class ExternalOpenBudget {
 public:
  using Clock = std::chrono::steady_clock;

  bool TryTake(Clock::time_point now,
               size_t limit = kMaxExternalOpensPerSecond) {
    while (!opens_.empty() && now - opens_.front() >= std::chrono::seconds(1))
      opens_.pop_front();
    if (opens_.size() >= limit)
      return false;
    opens_.push_back(now);
    return true;
  }

 private:
  std::deque<Clock::time_point> opens_;
};

namespace navigation_guard_internal {

inline std::string LowerAscii(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s)
    out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a')
                                         : c);
  return out;
}

// The URL's scheme, lowercased ("" when it has none).
inline std::string SchemeOf(const std::string& url) {
  size_t colon = url.find(':');
  if (colon == std::string::npos || colon == 0)
    return "";
  for (size_t i = 0; i < colon; ++i) {
    char c = url[i];
    bool ok =
        (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (i > 0 && ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'));
    if (!ok)
      return "";
  }
  return LowerAscii(url.substr(0, colon));
}

// about:blank / about:srcdoc, with any query or fragment.
inline bool IsBlankDocument(const std::string& url) {
  std::string lower = LowerAscii(url.substr(0, url.find_first_of("?#")));
  return lower == "about:blank" || lower == "about:srcdoc";
}

}  // namespace navigation_guard_internal

// The key a window's embedder-navigated URL gives it: its serialized origin,
// "file:" for a file: URL (whose origin is opaque) or an absolute path (the
// macOS backend loads one as a file URL), else "" (no origin of its own:
// data:, about:, a malformed URL).
inline std::string NavigationOriginKey(const std::string& url) {
  using namespace navigation_guard_internal;
  std::string scheme = SchemeOf(url);
  if (scheme == "file" ||
      (!url.empty() && url[0] == '/' && url.compare(0, 2, "//") != 0))
    return "file:";
  std::string origin = OriginOfUrl(url);
  return origin == kOpaqueOrigin ? std::string() : origin;
}

// Normalizes one allowed-origin entry: an origin ("scheme://host[:port]",
// optionally with a trailing "/"), serialized. Returns "" for anything else
// (a path, a query, a wildcard, an opaque origin).
inline std::string NormalizeAllowedNavigationOrigin(const std::string& entry) {
  std::string origin, scheme;
  bool any = false;
  if (!ParseBridgeOriginEntry(entry, &origin, &scheme, &any) || any ||
      !scheme.empty())
    return "";
  return origin;
}

// The decision for a top-level navigation to `url` in a window whose
// embedder-navigated key is `window_key` (NavigationOriginKey), with the
// embedder's extra `allowed` origins (normalized). Pure; see the file
// comment for the rules.
inline NavigationDecision DecideTopLevelNavigation(
    const std::string& window_key, const std::vector<std::string>& allowed,
    const std::string& url) {
  using namespace navigation_guard_internal;
  if (IsBlankDocument(url))
    return NavigationDecision::kAllow;
  std::string key = NavigationOriginKey(url);
  if (!key.empty()) {
    if (key == window_key)
      return NavigationDecision::kAllow;
    for (const std::string& origin : allowed) {
      if (origin == key)
        return NavigationDecision::kAllow;
    }
  }
  return IsAllowedExternalLinkUrl(url) ? NavigationDecision::kOpenInBrowser
                                       : NavigationDecision::kBlock;
}

// The process's guard state: each window's embedder-navigated key and the
// embedder's allowed origins. Thread-safe (the embedder navigates from its
// own thread; the engines decide on the UI thread).
class NavigationGuard {
 public:
  static NavigationGuard& Instance() {
    static NavigationGuard* guard = new NavigationGuard();
    return *guard;
  }

  // The embedder navigates `window_id` to `url`: its top-level frame may
  // load that URL's origin from now on (and nothing it allowed before).
  // Call BEFORE the engine starts the load, so the load's own check passes.
  void SetWindowUrl(uint32_t window_id, const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex_);
    window_keys_[window_id] = NavigationOriginKey(url);
  }

  void ForgetWindow(uint32_t window_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    window_keys_.erase(window_id);
    budgets_.erase(window_id);
  }

  // Replaces the origins every window may load (set_allowed_navigation_
  // origins). An entry that is not an origin is reported on stderr and
  // skipped. Returns how many were kept.
  size_t SetAllowedOrigins(const std::vector<std::string>& entries) {
    std::vector<std::string> kept;
    for (const std::string& entry : entries) {
      std::string origin = NormalizeAllowedNavigationOrigin(entry);
      if (origin.empty()) {
        std::cerr << "laufey: ignoring allowed navigation origin \"" << entry
                  << "\": not an origin (scheme://host[:port])" << std::endl;
        continue;
      }
      kept.push_back(origin);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    allowed_ = kept;
    return allowed_.size();
  }

  // The decision for a top-level navigation of `window_id` to `url`. A
  // refusal that would go to the browser counts against the window's
  // per-second budget and is kThrottled once that is spent.
  NavigationDecision Decide(
      uint32_t window_id, const std::string& url,
      ExternalOpenBudget::Clock::time_point now =
          ExternalOpenBudget::Clock::now()) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = window_keys_.find(window_id);
    NavigationDecision decision = DecideTopLevelNavigation(
        it == window_keys_.end() ? std::string() : it->second, allowed_, url);
    if (decision == NavigationDecision::kOpenInBrowser &&
        !budgets_[window_id].TryTake(now))
      return NavigationDecision::kThrottled;
    return decision;
  }

 private:
  NavigationGuard() = default;
  std::mutex mutex_;
  std::map<uint32_t, std::string> window_keys_;
  std::vector<std::string> allowed_;
  std::map<uint32_t, ExternalOpenBudget> budgets_;
};

// The message a cancelled navigation is logged with.
inline void LogGuardedNavigation(const std::string& url,
                                 NavigationDecision decision) {
  if (decision == NavigationDecision::kOpenInBrowser) {
    std::cerr << "laufey: " << url
              << " is not the app's origin: opening it in the browser instead"
              << std::endl;
  } else if (decision == NavigationDecision::kBlock) {
    std::cerr << "laufey: not loading " << url
              << " in the window: it is not the app's origin" << std::endl;
  } else if (decision == NavigationDecision::kThrottled) {
    std::cerr << "laufey: not opening " << url
              << " in the browser: the window asked for more than "
              << kMaxExternalOpensPerSecond << " in the last second"
              << std::endl;
  }
}

// --- Test hook: captured open-external requests (API 48) -----------------
//
// While capture is on, every URL a backend would hand to the OS browser
// (the guard, a popup, the reserved bridge call) is recorded instead of
// opened, so an e2e run checks what would have opened without launching a
// browser. Off by default; only the embedder (test_capture_external_opens)
// turns it on.
class ExternalOpenCapture {
 public:
  static ExternalOpenCapture& Instance() {
    static ExternalOpenCapture* capture = new ExternalOpenCapture();
    return *capture;
  }

  void SetEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    enabled_ = enabled;
    if (!enabled)
      urls_.clear();
  }

  // True (and `url` recorded) when capture is on: the caller must not open
  // it.
  bool Capture(const std::string& url) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!enabled_)
      return false;
    urls_.push_back(url);
    return true;
  }

  std::vector<std::string> Take() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    out.swap(urls_);
    return out;
  }

 private:
  ExternalOpenCapture() = default;
  std::mutex mutex_;
  bool enabled_ = false;
  std::vector<std::string> urls_;
};

// `urls` as a JSON array of strings.
inline std::string ExternalOpensJson(const std::vector<std::string>& urls) {
  static const char kHex[] = "0123456789abcdef";
  std::string out = "[";
  for (size_t i = 0; i < urls.size(); ++i) {
    if (i)
      out += ",";
    out += "\"";
    for (unsigned char c : urls[i]) {
      if (c == '"' || c == '\\') {
        out += '\\';
        out += static_cast<char>(c);
      } else if (c < 0x20) {
        out += "\\u00";
        out += kHex[c >> 4];
        out += kHex[c & 15];
      } else {
        out += static_cast<char>(c);
      }
    }
    out += "\"";
  }
  out += "]";
  return out;
}

// --- The C ABI entry points (API 48), shared by every web-engine backend ---

inline void SetAllowedNavigationOriginsForAbi(const char* const* origins,
                                              size_t count) {
  std::vector<std::string> entries;
  for (size_t i = 0; origins && i < count; ++i) {
    if (origins[i])
      entries.emplace_back(origins[i]);
  }
  NavigationGuard::Instance().SetAllowedOrigins(entries);
}

inline void TestCaptureExternalOpensForAbi(bool capture) {
  ExternalOpenCapture::Instance().SetEnabled(capture);
}

// The recorded URLs as a malloc'd JSON array (freed with string_free).
inline char* TestTakeExternalOpensForAbi() {
  std::string json = ExternalOpensJson(ExternalOpenCapture::Instance().Take());
  char* out = static_cast<char*>(std::malloc(json.size() + 1));
  if (out)
    std::memcpy(out, json.c_str(), json.size() + 1);
  return out;
}

}  // namespace laufey_common

#endif  // LAUFEY_NAVIGATION_GUARD_H_
