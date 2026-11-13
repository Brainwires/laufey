// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Child web views (API 58): the rules every backend shares, with no OS API,
// so they are tested once (webview_test.cc). See docs/webviews.md.
//
//   - ParseWebViewOptions: create_webview's options JSON, validated (status
//     LAUFEY_WEBVIEW_INVALID / NOT_ALLOWED_URL with a message).
//   - IsAllowedWebViewUrl / IsAllowedWebViewNavigation: what a web view may
//     load (http(s) with a host; the view's own navigations may also be
//     about:blank).
//   - Partitions: IsValidPartitionName, IsEphemeralPartition,
//     PartitionDirName (a directory name under the app data directory),
//     PartitionUuid (a stable UUID per name, for engines that key stores by
//     UUID: WKWebsiteDataStore).
//   - The JSON of each event and of webview_state.
//   - kWebViewEvalWrapper / WebViewEvalScript: the script engines without a
//     protocol-level evaluate run (WKWebView, WebKitGTK), and
//     ParseWebViewEvalEnvelope, which reads its answer back.
//   - WebViewSinks: id allocation, each view's window and event sink, the
//     per-window limit, and the exactly-once CLOSED.

#ifndef LAUFEY_WEBVIEW_H_
#define LAUFEY_WEBVIEW_H_

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "laufey.h"

namespace laufey_common {

enum class WebViewPopups { kSameView, kBrowser, kDeny };
enum class WebViewDownloads { kDeny, kAsk, kBrowser };
enum class WebViewPermissions { kDeny, kAsk };

constexpr const char* kDefaultWebViewPartition = "webview:default";

struct WebViewBounds {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

struct WebViewOptions {
  std::string url;
  WebViewBounds bounds;
  std::string partition = kDefaultWebViewPartition;
  bool visible = true;
  WebViewPopups popups = WebViewPopups::kSameView;
  WebViewDownloads downloads = WebViewDownloads::kDeny;
  WebViewPermissions permissions = WebViewPermissions::kDeny;
  std::string user_agent;  // empty: the engine's own
};

// Parses and validates create_webview's options. Returns LAUFEY_WEBVIEW_OK,
// LAUFEY_WEBVIEW_INVALID (bad JSON, a missing / mistyped / unknown-valued
// field, bad bounds or partition; `error` says which) or
// LAUFEY_WEBVIEW_NOT_ALLOWED_URL.
int32_t ParseWebViewOptions(const std::string& json, WebViewOptions* out,
                            std::string* error);

// Finite, width / height >= 0, and every value within +-1e6 CSS pixels.
bool IsValidWebViewBounds(double x, double y, double width, double height);

// What create_webview / webview_navigate accept: an absolute http(s) URL
// with a host, no whitespace or control characters
// (IsAllowedExternalLinkUrl's rule).
bool IsAllowedWebViewUrl(const std::string& url);

// What the view's own navigations (links, scripts, redirects) may load:
// IsAllowedWebViewUrl, plus about:blank / about:srcdoc. Anything else
// (file:, data:, javascript:, the app's custom schemes, mailto:) is
// cancelled.
bool IsAllowedWebViewNavigation(const std::string& url);

// 1..64 characters of [A-Za-z0-9._:-], the first alphanumeric.
bool IsValidPartitionName(const std::string& name);

// "ephemeral" or "ephemeral:<anything>": kept in memory only.
bool IsEphemeralPartition(const std::string& name);

// A directory name for a persistent partition (':' becomes '~', which a
// valid name never contains, so distinct names never share a directory).
std::string PartitionDirName(const std::string& name);

// A stable, name-derived RFC 4122 version-5-shaped UUID (16 bytes).
std::array<uint8_t, 16> PartitionUuid(const std::string& name);
// The same as "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", upper case.
std::string PartitionUuidString(const std::string& name);

// --- Event and state JSON -------------------------------------------------

std::string WebViewTitleJson(const std::string& title);
std::string WebViewUrlJson(const std::string& url, bool can_go_back,
                           bool can_go_forward);
// `state` is one of "loading", "interactive", "complete", "failed"; the
// error fields are written only for "failed".
std::string WebViewLoadJson(const char* state, const std::string& url,
                            const char* error_code = nullptr,
                            const std::string& error_message = std::string());
// `level` one of "log", "info", "warn", "error", "debug" (anything else is
// written as "log"); `line` <= 0 and an empty `source` are left out.
std::string WebViewConsoleJson(const char* level, const std::string& message,
                               const std::string& source, int line);
std::string WebViewFocusJson(bool focused);
std::string WebViewNewWindowJson(const std::string& url, bool popup);

struct WebViewStateFields {
  std::string url;
  std::string title;
  const char* load = "complete";
  bool can_go_back = false;
  bool can_go_forward = false;
  bool visible = true;
  WebViewBounds bounds;
};
std::string WebViewStateJson(const WebViewStateFields& s);

// --- Evaluate ---------------------------------------------------------------
//
// For engines that run a function body with `await` but have no
// protocol-level evaluate: WebViewEvalScript(script) is an async function
// body that evaluates `script` (an indirect eval: the completion value of a
// classic script), awaits it, serializes it, and returns an envelope string:
//   {"ok":true,"json":"<the JSON>"}
//   {"ok":false,"kind":"error"|"not_serializable","message":"..."}
// The serializer refuses functions, symbols, bigints, DOM nodes, windows and
// cycles (not_serializable); undefined becomes null.
std::string WebViewEvalScript(const std::string& script);

// The serializer alone: a JS function expression `(value) => string` that
// throws a TypeError whose message starts with "not_serializable: " for a
// value JSON can't hold. Protocol-level evaluates (DevTools'
// Runtime.callFunctionOn) call it on the script's result.
extern const char kWebViewSerializeFunction[];

// Reads an envelope back: LAUFEY_WEBVIEW_OK with `text` the JSON, or
// SCRIPT_ERROR / NOT_SERIALIZABLE with `text` the message, or FAILED when
// `envelope` is not one.
int32_t ParseWebViewEvalEnvelope(const std::string& envelope,
                                 std::string* text);

// --- Registry ---------------------------------------------------------------
//
// Ids are unique in the process and never reused. Emit and Close call the
// sink with the registry's lock held (a recursive mutex: a sink may call back
// into the registry), so a sink must not block; Close emits CLOSED exactly
// once and forgets the id, after which Emit for it does nothing.
class WebViewSinks {
 public:
  static WebViewSinks& Instance();

  // Allocates an id for a view of `window_id` with this sink, or returns 0
  // when the window already holds LAUFEY_WEBVIEW_MAX_PER_WINDOW views.
  uint32_t Add(uint32_t window_id, laufey_webview_event_fn fn, void* user_data);
  bool Has(uint32_t webview_id);
  // The window of `webview_id`, or 0.
  uint32_t WindowOf(uint32_t webview_id);
  size_t CountForWindow(uint32_t window_id);
  std::vector<uint32_t> ViewsOfWindow(uint32_t window_id);
  void Emit(uint32_t webview_id, int32_t kind, const std::string& detail);
  // Emits CLOSED and forgets the id. False when unknown (already closed).
  bool Close(uint32_t webview_id);
  // Drops an id without an event (a creation that failed after Add).
  void Abandon(uint32_t webview_id);

  // Test-only.
  void ResetForTest();

 private:
  struct Sink {
    uint32_t window_id;
    laufey_webview_event_fn fn;
    void* user_data;
  };
  std::recursive_mutex mu_;
  std::map<uint32_t, Sink> sinks_;
  uint32_t next_id_ = 1;
};

}  // namespace laufey_common

#endif  // LAUFEY_WEBVIEW_H_
