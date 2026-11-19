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
//   - Keys: ParseWebViewForwardKeys ("forwardKeys", accelerator strings; a
//     leading '!' puts an entry before the app menu's accelerators),
//     WebViewKeyAccelerator / WebViewKeyMatches / WebViewKeyMatchesBeforeMenu
//     (a keydown, as DOM `code` and modifiers, against them), the KEY event's
//     JSON, and WebViewHeldKeys (the forwarded keydowns whose keyup is owed
//     to the app).
//   - Downloads and permission requests the app decides (API 59,
//     "downloads": "app" / "permissions": "app"): CheckWebViewOptionsSupported
//     (an engine without them refuses the option), the DOWNLOAD and
//     PERMISSION_REQUEST events' JSON, the ids, which
//     webview_download_control action each download phase takes
//     (WebViewDownloadAccepts), the SAVE path check
//     (IsValidWebViewDownloadPath) and the progress throttle.
//   - WebViewSinks: id allocation, each view's window and event sink, the
//     per-window limit, and the exactly-once CLOSED.

#ifndef LAUFEY_WEBVIEW_H_
#define LAUFEY_WEBVIEW_H_

#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "laufey.h"
#include "laufey_system.h"

namespace laufey_common {

enum class WebViewPopups { kSameView, kBrowser, kDeny };
// kApp (API 59): the app decides each download / permission request from the
// view's DOWNLOAD / PERMISSION_REQUEST events.
enum class WebViewDownloads { kDeny, kAsk, kBrowser, kApp };
enum class WebViewPermissions { kDeny, kAsk, kApp };

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
  std::string user_agent;         // empty: the engine's own
  bool app_context_menu = false;  // "contextMenu": "app"
  bool clipboard_read = false;
  bool clipboard_write = true;  // on a user gesture
  // "forwardKeys", each in its canonical form (CanonicalAccelerator), with
  // its leading '!' when it goes before the app menu's accelerators.
  std::vector<std::string> forward_keys;
};

// "forwardKeys" limits: entries, and characters per entry.
constexpr size_t kMaxWebViewForwardKeys = 64;
constexpr size_t kMaxWebViewForwardKeyLength = 64;

// Parses webview_set_forward_keys' JSON array of accelerator strings (the
// menu / global shortcut syntax, ParseAccelerator; CmdOrCtrl is Command on
// macOS, Ctrl elsewhere) into their canonical forms, duplicates dropped. A
// leading '!' ("!CmdOrCtrl+W") is kept on the canonical form: that entry is
// matched before the app menu's accelerators (WebViewKeyMatchesBeforeMenu).
// LAUFEY_WEBVIEW_OK or LAUFEY_WEBVIEW_INVALID (not an array of strings, more
// than kMaxWebViewForwardKeys entries, one longer than
// kMaxWebViewForwardKeyLength or not an accelerator; `error` says which).
int32_t ParseWebViewForwardKeys(const std::string& json_array,
                                std::vector<std::string>* out,
                                std::string* error);

// Parses and validates create_webview's options. Returns LAUFEY_WEBVIEW_OK,
// LAUFEY_WEBVIEW_INVALID (bad JSON, a missing / mistyped / unknown-valued
// field, bad bounds or partition; `error` says which) or
// LAUFEY_WEBVIEW_NOT_ALLOWED_URL.
int32_t ParseWebViewOptions(const std::string& json, WebViewOptions* out,
                            std::string* error);

// LAUFEY_WEBVIEW_OK, or LAUFEY_WEBVIEW_INVALID with a message when the
// options ask for "downloads": "app" or "permissions": "app" and
// `capabilities` (the backend's webview_capabilities) lacks
// LAUFEY_WEBVIEW_CAP_DOWNLOAD_EVENTS / _PERMISSION_REQUESTS: an app that
// meant to decide them itself must never get the engine's default instead.
// Every backend calls it right after ParseWebViewOptions.
int32_t CheckWebViewOptionsSupported(const WebViewOptions& options,
                                     uint32_t capabilities, std::string* error);

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
// `reason` (nullptr: left out) says why laufey moved the focus itself:
// kWebViewFocusHidden (the focused view was hidden) or kWebViewFocusBlur
// (webview_command BLUR).
std::string WebViewFocusJson(bool focused, const char* reason = nullptr);
constexpr const char* kWebViewFocusHidden = "hidden";
constexpr const char* kWebViewFocusBlur = "blur";
std::string WebViewNewWindowJson(const std::string& url, bool popup);
std::string WebViewFindResultJson(int matches, int active_match);

struct WebViewContextMenuFields {
  double x = 0;
  double y = 0;
  std::string link_url;        // empty: left out
  std::string image_url;       // empty: left out
  std::string selection_text;  // empty: left out
  bool is_editable = false;
  const char* media_type = nullptr;  // "image", "video", "audio", "canvas",
                                     // "plugin"; nullptr: left out
};
std::string WebViewContextMenuJson(const WebViewContextMenuFields& f);

// The KEY event: a keydown matching "forwardKeys" (`forwarded`), the keyup
// of such a keydown (`type` "keyup", `forwarded`), or a keydown the page did
// not handle.
struct WebViewKeyFields {
  const char* type = "keydown";  // "keydown" or "keyup"
  std::string key;               // DOM KeyboardEvent.key
  std::string code;              // DOM KeyboardEvent.code
  bool alt_key = false;
  bool ctrl_key = false;
  bool meta_key = false;  // Command on macOS, the Windows / Super key elsewhere
  bool shift_key = false;
  bool repeat = false;
  bool forwarded = false;
};
std::string WebViewKeyJson(const WebViewKeyFields& f);

// The accelerator a keydown is (its DOM `code` names the physical key, as
// accelerators do; `meta` is kModSuper), or false when the code names no
// accelerator key (a modifier, an unmapped key, "Unidentified").
bool WebViewKeyAccelerator(const std::string& code, bool alt, bool ctrl,
                           bool meta, bool shift, Accelerator* out);

// Whether that keydown is one of `forward_keys` (canonical forms, as
// ParseWebViewForwardKeys returns them, with or without the '!').
bool WebViewKeyMatches(const std::vector<std::string>& forward_keys,
                       const std::string& code, bool alt, bool ctrl, bool meta,
                       bool shift);
// Whether it is one of the entries with a leading '!': those are matched
// before the app menu's accelerators (the others after them).
bool WebViewKeyMatchesBeforeMenu(const std::vector<std::string>& forward_keys,
                                 const std::string& code, bool alt, bool ctrl,
                                 bool meta, bool shift);

// The forwarded keydowns still held: each one's keyup goes to the app too
// (its page never saw the keydown), matched by the physical key (`code`)
// whatever modifiers are down by then. A view whose keyboard focus goes
// (or whose app is deactivated) releases them all at once, so a keyup that
// went elsewhere still ends the hold.
class WebViewHeldKeys {
 public:
  // Notes a forwarded keydown (a repeat of a held key changes nothing).
  void Press(const std::string& code, const std::string& key);
  // The keyup of `code`: true (with the keydown's `key`) when it was held,
  // and it no longer is.
  bool Release(const std::string& code, std::string* key);
  // The keyup fields of every held key (no modifiers, forwarded), oldest
  // first; none is held afterwards.
  std::vector<WebViewKeyFields> ReleaseAll();
  bool empty() const {
    return keys_.empty();
  }

 private:
  std::vector<std::pair<std::string, std::string>> keys_;  // code, key
};

// --- Downloads the app decides (API 59) ------------------------------------

// A process-unique download id (never 0, never reused). Any thread.
uint32_t NextWebViewDownloadId();

// The DOWNLOAD event's "state".
constexpr const char* kWebViewDownloadRequested = "requested";
constexpr const char* kWebViewDownloadProgressing = "progressing";
constexpr const char* kWebViewDownloadCompleted = "completed";
constexpr const char* kWebViewDownloadCancelled = "cancelled";
constexpr const char* kWebViewDownloadInterrupted = "interrupted";
// The "reason" of an interrupted download.
constexpr const char* kWebViewDownloadReasonNetwork = "network";
constexpr const char* kWebViewDownloadReasonServer = "server";
constexpr const char* kWebViewDownloadReasonFile = "file";
constexpr const char* kWebViewDownloadReasonCrash = "crash";
constexpr const char* kWebViewDownloadReasonFailed = "failed";

struct WebViewDownloadFields {
  uint32_t id = 0;
  const char* state = kWebViewDownloadRequested;
  std::string url;
  // The engine's suggestion; written through SanitizeDownloadFileName
  // (laufey_downloads.h), so always a bare file name.
  std::string filename;
  std::string mime_type;       // empty: left out
  int64_t total_bytes = -1;    // < 0: unknown, left out
  int64_t received_bytes = 0;  // < 0 is written as 0
  std::string path;            // empty: left out
  bool paused = false;         // false: left out
  // "canResume": written for "progressing" (where it is known) and
  // "interrupted"; < 0: left out.
  int can_resume = -1;
  const char* reason = nullptr;  // "interrupted" only; nullptr: "failed"
};
std::string WebViewDownloadJson(const WebViewDownloadFields& f);

// Where a download the app decides is in its life, as
// webview_download_control sees it.
enum class WebViewDownloadPhase {
  kRequested,    // "requested" sent; waiting for SAVE / SAVE_AS / CANCEL
  kChoosing,     // SAVE_AS: the save panel is up
  kProgressing,  // being written
  kPaused,       // paused by PAUSE
  kInterrupted,  // "interrupted" with "canResume": true
  kDone,         // its last event was sent
};
// LAUFEY_WEBVIEW_OK when a download in `phase` takes `action`
// (LAUFEY_WEBVIEW_DOWNLOAD_*), LAUFEY_WEBVIEW_UNSUPPORTED for PAUSE / RESUME
// when `can_pause` is false (the engine lacks
// LAUFEY_WEBVIEW_CAP_DOWNLOAD_PAUSE), else LAUFEY_WEBVIEW_INVALID:
//   SAVE, SAVE_AS  kRequested
//   CANCEL         every phase but kDone
//   PAUSE          kProgressing
//   RESUME         kPaused, kInterrupted
int32_t WebViewDownloadAccepts(WebViewDownloadPhase phase, int32_t action,
                               bool can_pause);

// What SAVE's `path` must be: UTF-8 without control characters, absolute
// (a drive or UNC path on Windows), at most 4096 bytes, not ending in a
// separator, in a directory that exists, and not itself a directory or a
// symbolic link (the engine would write through it). `error` says which.
bool IsValidWebViewDownloadPath(const std::string& path, std::string* error);
// SAVE's path as the engines take it: a Windows verbatim path
// ("\\\\?\\C:\\dir\\f", "\\\\?\\UNC\\server\\share\\f"; what a path
// canonicalized by Rust or Win32 looks like) without its prefix, which the
// engines' own path handling refuses. Any other path is returned unchanged.
std::string NormalizeWebViewDownloadPath(const std::string& path);
// The same without touching the file system (the syntax rules only), for
// tests of the other platform's rules.
bool IsWellFormedWebViewDownloadPath(const std::string& path, bool windows,
                                     std::string* error);

// "progressing" events: at most one per kWebViewDownloadProgressInterval
// per download. Ready() is true the first time and then once the interval
// has passed since the last true; a pause, resume or final event is sent
// without asking.
constexpr int kWebViewDownloadProgressIntervalMs = 200;
class WebViewProgressThrottle {
 public:
  bool Ready(int64_t now_ms);

 private:
  bool sent_ = false;
  int64_t last_ms_ = 0;
};

// --- Permission requests the app answers (API 59) ----------------------------

// A process-unique permission request id (never 0, never reused). Any thread.
uint32_t NextWebViewPermissionId();

// The names in PERMISSION_REQUEST's "permissions".
constexpr const char* kWebViewPermissionCamera = "camera";
constexpr const char* kWebViewPermissionMicrophone = "microphone";
constexpr const char* kWebViewPermissionScreen = "screen";
constexpr const char* kWebViewPermissionGeolocation = "geolocation";
constexpr const char* kWebViewPermissionNotifications = "notifications";
constexpr const char* kWebViewPermissionMidi = "midi";
constexpr const char* kWebViewPermissionPointerLock = "pointer-lock";
constexpr const char* kWebViewPermissionStorageAccess = "storage-access";
constexpr const char* kWebViewPermissionSensors = "sensors";
constexpr const char* kWebViewPermissionClipboardRead = "clipboard-read";
constexpr const char* kWebViewPermissionWindowManagement = "window-management";
constexpr const char* kWebViewPermissionLocalFonts = "local-fonts";
constexpr const char* kWebViewPermissionFileSystem = "file-system";
constexpr const char* kWebViewPermissionMultipleDownloads =
    "multiple-downloads";
constexpr const char* kWebViewPermissionAutoplay = "autoplay";

struct WebViewPermissionFields {
  uint32_t id = 0;
  std::string origin;  // the requesting origin ("https://example.com")
  std::vector<std::string> permissions;  // kWebViewPermission*, in this order
  // The frame that asked, where the engine says: < 0 unknown ("frame" is
  // left out), 0 a subframe, 1 the main frame.
  int frame_is_main = -1;
  std::string frame_origin;  // empty: left out
};
std::string WebViewPermissionRequestJson(const WebViewPermissionFields& f);
// {"id": id, "state": "cancelled"}
std::string WebViewPermissionCancelledJson(uint32_t id);

// The origin of an absolute URL as PERMISSION_REQUEST reports it:
// "scheme://host[:port]" in lower case with the scheme's default port left
// out, the URL's path, query, fragment and credentials dropped. "" when
// `url` has no host (about:blank, data:).
std::string WebViewOriginOf(const std::string& url);

// Zoom factors webview_set_zoom accepts.
bool IsValidWebViewZoom(double factor);

struct WebViewStateFields {
  std::string url;
  std::string title;
  const char* load = "complete";
  bool can_go_back = false;
  bool can_go_forward = false;
  bool visible = true;
  WebViewBounds bounds;
  int32_t z_order = 0;
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

// Before it runs the script, WebViewEvalScript checks that the page lets
// `eval` run (a Content-Security-Policy without 'unsafe-eval' refuses it in
// every world): when it doesn't, the envelope is
//   {"ok":false,"kind":"eval_blocked","message":"..."}
// and nothing of the script ran, so the engine runs it again through its own
// evaluate, which no page CSP restricts (WKWebView's evaluateJavaScript:
// inFrame:inContentWorld:, WebKitGTK's webkit_web_view_evaluate_javascript).
// Never an embedder status: ParseWebViewEvalEnvelope's answer for it only.
constexpr int32_t kWebViewEvalBlocked = -1;
// Reads an envelope back: LAUFEY_WEBVIEW_OK with `text` the JSON, or
// SCRIPT_ERROR / NOT_SERIALIZABLE with `text` the message,
// kWebViewEvalBlocked, or FAILED when `envelope` is not one.
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

  // Stacking among a window's views (webview_set_z_order): Add gives one
  // more than the window's highest. False for an unknown id.
  bool SetZOrder(uint32_t webview_id, int32_t z_order);
  bool Raise(uint32_t webview_id);  // one more than the window's highest
  bool Lower(uint32_t webview_id);  // one less than the window's lowest
  int32_t ZOrderOf(uint32_t webview_id);
  // The window's views bottom to top (z order, then creation order).
  std::vector<uint32_t> StackOrder(uint32_t window_id);

  // Test-only.
  void ResetForTest();

 private:
  struct Sink {
    uint32_t window_id;
    laufey_webview_event_fn fn;
    void* user_data;
    int32_t z_order;
  };
  std::recursive_mutex mu_;
  std::map<uint32_t, Sink> sinks_;
  uint32_t next_id_ = 1;
};

}  // namespace laufey_common

#endif  // LAUFEY_WEBVIEW_H_
