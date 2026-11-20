// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Child web views on WebView2 (API 58); see child_webview_win.h and
// docs/webviews.md.
//
// Each view is its own ICoreWebView2Controller, created from the window's
// environment with ICoreWebView2Environment10::
// CreateCoreWebView2ControllerWithOptions in a profile named after its
// partition (never the app's own profile; InPrivate for an ephemeral
// partition). Its parent is a host window of its own, a child of the
// window's HWND placed above the page's controller, so the view draws over
// the page and its bounds are the host's.
//
// Isolation: the view's web view gets no init script, no WebMessageReceived
// handler (web messages and host objects are off), no WebResourceRequested
// filter (the environment's custom schemes are served only for the page's
// controllers) and none of the window's NavigationStarting / will-navigate
// handlers, which belong to the page's web view. Its own NavigationStarting
// cancels anything IsAllowedWebViewNavigation refuses.
//
// Evaluate and capture go through the DevTools protocol
// (CallDevToolsProtocolMethod), as on CEF: Runtime.evaluate in an isolated
// world made with Page.createIsolatedWorld (no page CSP applies), and
// Page.captureScreenshot. Console messages come from Runtime.consoleAPICalled
// and Runtime.exceptionThrown.
//
// Keys: the controller's AcceleratorKeyPressed gives the "forwardKeys" marked
// '!' the key first, then the app menu's accelerators
// (win32_menu::HandleAcceleratorKey, as the page's controller does), then the
// other "forwardKeys"; any of them marks it handled. A forwarded keydown's
// keyup is the app's too: AcceleratorKeyPressed's KEY_UP when WebView2 raises
// one, else the host's timer, which runs only while a forwarded key is held
// and reads the key's state (GetAsyncKeyState) every kHeldKeyPollMs; a view
// that loses the focus releases them all. WebView2
// raises it only for "accelerator" keys (Ctrl or Alt held, a key that types no
// character, Escape), so a forward key that types a character with no Ctrl /
// Alt (Shift+L, Meta+L) reaches the page. WebView2 tells nothing of the keys
// the page did not handle (no LAUFEY_WEBVIEW_CAP_UNHANDLED_KEYS). Focus: a
// mouse button pressed in the view's windows notifies its host
// (WM_PARENTNOTIFY) as the press is dispatched, which reports FOCUS ahead of
// the controller's GotFocus. Hiding a focused view (and BLUR) moves the focus
// to the page's controller (MoveFocus, in the foreground window only: no
// activation): FOCUS false with its "reason".
//
// "downloads": "app" (API 59): DownloadStarting is handled (no WebView2 UI)
// and held open with its deferral until webview_download_control answers:
// SAVE sets ResultFilePath, SAVE_AS asks with the save dialog the window's
// "ask" policy uses (AskDownloadDestination), CANCEL cancels the event. Then
// the ICoreWebView2DownloadOperation's BytesReceivedChanged / StateChanged
// report it; WebView2 pauses by interrupting with USER_PAUSED, which is
// reported as paused, not as an interruption. StateChanged can come late or
// never on a loaded machine, so a decided download's state is also polled
// (OnDownloadPoll) and its end reported once, whichever sees it first; the
// view's SmartScreen reputation check (which holds a completion) is off.
//
// "permissions": "app" (API 59): PermissionRequested is held open with its
// deferral until webview_permission_respond answers (never remembered in the
// partition's profile where SavesInProfile exists). A subframe's request is
// raised first on its ICoreWebView2Frame (FrameCreated +
// ICoreWebView2Frame3), handled there as the frame's ("isMain": false), so
// one reaching the CoreWebView2 is the main frame's; when a frame can't take
// that handler the frame is left out. A new main-frame navigation and closing
// the view refuse the requests still waiting.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "child_webview_win.h"

#include <shellapi.h>
#include <wrl.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "laufey_backend_common.h"
#include "laufey_downloads.h"
#include "laufey_json.h"
#include "laufey_launch_config.h"
#include "laufey_navigation_guard.h"
#include "laufey_notifications.h"
#include <win32_menu.h>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace laufey_child_webview_win {

namespace {

using laufey_common::WebViewOptions;
using laufey_common::WebViewSinks;

// A DevTools method's answer: `success` and the result (or error) object,
// always a dictionary.
using DevToolsCallback =
    std::function<void(bool success, laufey::ValuePtr result)>;

constexpr UINT kDevToolsTimeoutMs = 15000;
constexpr int kConsolePerSecond = 200;
constexpr size_t kConsoleMaxBytes = 16 * 1024;
constexpr wchar_t kHostClass[] = L"LaufeyChildWebViewHost";
constexpr char kEvalGroup[] = "laufey-webview-eval";

// A download of a view under "downloads": "app" (API 59).
struct ChildDownload {
  uint32_t id = 0;
  laufey_common::WebViewDownloadPhase phase =
      laufey_common::WebViewDownloadPhase::kRequested;
  // The DownloadStarting event held open until the download is decided.
  ComPtr<ICoreWebView2DownloadStartingEventArgs> args;
  ComPtr<ICoreWebView2Deferral> deferral;
  ComPtr<ICoreWebView2DownloadOperation> operation;
  EventRegistrationToken bytes_token = {};
  EventRegistrationToken state_token = {};
  bool subscribed = false;
  std::string url;
  std::string filename;  // sanitized
  std::string mime_type;
  std::string path;          // once decided
  int64_t total_bytes = -1;  // < 0: unknown
  laufey_common::WebViewProgressThrottle throttle;
  // The state poll behind StateChanged (see OnDownloadPoll), armed once the
  // download is decided.
  laufey_common::WebViewDownloadPoll poll;
  // Pause() is asynchronous: WebView2 pauses by interrupting with
  // USER_PAUSED some time later, and a Resume() before that does nothing.
  // pause_in_flight: PAUSE was asked and its interruption not seen yet;
  // resume_pending: RESUME came meanwhile, applied when it is;
  // resume_in_flight: Resume() was called and the download is not back in
  // progress yet (an interruption state seen meanwhile is the old pause).
  bool pause_in_flight = false;
  bool resume_pending = false;
  bool resume_in_flight = false;
};

// A permission request of a view under "permissions": "app" (API 59), held
// open until the app answers.
struct ChildPermission {
  ComPtr<ICoreWebView2PermissionRequestedEventArgs> args;
  ComPtr<ICoreWebView2Deferral> deferral;
};

struct ChildView {
  uint32_t id = 0;
  uint32_t window_id = 0;
  HWND parent = nullptr;
  // The view's own child window of `parent`: its controller's parent.
  HWND host = nullptr;
  // The window's page controller (its bounds and scale place the view).
  ComPtr<ICoreWebView2Controller> page;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
  ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> console_receiver;
  ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> exception_receiver;
  EventRegistrationToken console_token = {};
  EventRegistrationToken exception_token = {};
  WebViewOptions options;
  // The URL to load once the controller exists (and the state's URL until
  // the first URL event).
  std::string pending_url;
  bool creating = false;    // the controller is being created
  bool first_load = false;  // the first navigation was started
  const char* load = "complete";
  UINT64 navigation = 0;  // the main-frame navigation the events are for
  // A download started since that navigation began (it ends the navigation
  // without a page, which is no failed load).
  bool download_started = false;
  std::string title;
  laufey_common::WebViewBounds bounds;
  bool visible = true;
  bool focused = false;
  // Why laufey is moving the focus out (kWebViewFocusHidden / Blur).
  const char* focus_reason = nullptr;
  // Forwarded keydowns whose keyup goes to the app too, with each one's
  // virtual key (the host's timer polls it).
  laufey_common::WebViewHeldKeys held_keys;
  std::map<std::string, UINT> held_vk;
  std::string sent_url;
  bool sent_back = false;
  bool sent_forward = false;
  bool url_sent = false;
  int64_t console_second = 0;
  int console_count = 0;
  int next_call = 1;
  std::map<int, DevToolsCallback> pending;
  // "downloads": "app" / "permissions": "app" (API 59), by id.
  std::map<uint32_t, std::unique_ptr<ChildDownload>> downloads;
  std::map<uint32_t, ChildPermission> permissions;
  // Every subframe so far took the frame-level PermissionRequested handler,
  // so a request reaching the CoreWebView2 is the main frame's.
  bool frames_attributed = false;
};

// UI thread only.
std::map<uint32_t, std::unique_ptr<ChildView>>& Views() {
  static auto* views = new std::map<uint32_t, std::unique_ptr<ChildView>>();
  return *views;
}

ChildView* Find(uint32_t id) {
  auto it = Views().find(id);
  return it == Views().end() ? nullptr : it->second.get();
}

// DevTools timeouts: SetTimer id -> (view, call). UI thread only.
std::map<UINT_PTR, std::pair<uint32_t, int>>& Timers() {
  static auto* timers = new std::map<UINT_PTR, std::pair<uint32_t, int>>();
  return *timers;
}

// Whether the environments have profiles: 0 not known yet, 1 yes, 2 no.
std::atomic<int> g_env_profiles{0};

std::string Utf8(LPCWSTR s) {
  return s ? laufey_common::WideToUtf8(s) : std::string();
}

std::wstring Wide(const std::string& s) {
  return laufey_common::Utf8ToWide(s);
}

// A JSON string literal.
std::string Quote(const std::string& s) {
  return "\"" + json::Escape(s) + "\"";
}

void Emit(uint32_t id, int32_t kind, const std::string& detail) {
  WebViewSinks::Instance().Emit(id, kind, detail);
}

void SetLoad(ChildView* v, const char* state, const std::string& url,
             const char* code = nullptr, const std::string& message = "") {
  v->load = state;
  Emit(v->id, LAUFEY_WEBVIEW_EVENT_LOAD,
       laufey_common::WebViewLoadJson(state, url, code, message));
}

// --- Values ------------------------------------------------------------------

laufey::ValuePtr Field(const laufey::ValuePtr& dict, const char* key) {
  if (!dict || !dict->IsDict())
    return nullptr;
  const laufey::ValueDict& d = dict->GetDict();
  auto it = d.find(key);
  return it == d.end() ? nullptr : it->second;
}

bool HasString(const laufey::ValuePtr& dict, const char* key) {
  laufey::ValuePtr v = Field(dict, key);
  return v && v->IsString();
}

std::string StringField(const laufey::ValuePtr& dict, const char* key) {
  laufey::ValuePtr v = Field(dict, key);
  return v && v->IsString() ? v->GetString() : std::string();
}

bool IntField(const laufey::ValuePtr& dict, const char* key, int* out) {
  laufey::ValuePtr v = Field(dict, key);
  if (v && v->IsInt()) {
    *out = v->GetInt();
    return true;
  }
  if (v && v->IsDouble() && std::isfinite(v->GetDouble())) {
    *out = static_cast<int>(v->GetDouble());
    return true;
  }
  return false;
}

laufey::ValuePtr ErrorDict(const char* message) {
  laufey::ValuePtr d = laufey::Value::Dict();
  d->GetDict()["message"] = laufey::Value::String(message);
  return d;
}

std::string ErrorMessage(const laufey::ValuePtr& d, const char* fallback) {
  std::string message = StringField(d, "message");
  return message.empty() ? std::string(fallback) : message;
}

// The text of a Runtime exceptionDetails object.
std::string ExceptionText(const laufey::ValuePtr& details) {
  if (!details || !details->IsDict())
    return "the script threw";
  laufey::ValuePtr e = Field(details, "exception");
  if (e && e->IsDict()) {
    if (HasString(e, "description")) {
      std::string d = StringField(e, "description");
      // The first line ("Error: x"), not the stack.
      size_t nl = d.find('\n');
      return nl == std::string::npos ? d : d.substr(0, nl);
    }
    if (HasString(e, "value"))
      return StringField(e, "value");
  }
  if (HasString(details, "text"))
    return StringField(details, "text");
  return "the script threw";
}

// --- Navigation --------------------------------------------------------------

std::string CurrentUrl(ChildView* v) {
  if (!v->webview)
    return v->pending_url;
  LPWSTR source = nullptr;
  std::string url;
  if (SUCCEEDED(v->webview->get_Source(&source)) && source)
    url = Utf8(source);
  if (source)
    CoTaskMemFree(source);
  return url;
}

void MaybeSendUrl(ChildView* v) {
  if (!v->webview)
    return;
  std::string url = CurrentUrl(v);
  if (url.empty() || (url == "about:blank" && !v->url_sent))
    return;  // the blank page every web view starts on
  BOOL back = FALSE, forward = FALSE;
  v->webview->get_CanGoBack(&back);
  v->webview->get_CanGoForward(&forward);
  if (v->url_sent && url == v->sent_url && (back != FALSE) == v->sent_back &&
      (forward != FALSE) == v->sent_forward)
    return;
  v->url_sent = true;
  v->sent_url = url;
  v->sent_back = back != FALSE;
  v->sent_forward = forward != FALSE;
  Emit(v->id, LAUFEY_WEBVIEW_EVENT_URL,
       laufey_common::WebViewUrlJson(url, v->sent_back, v->sent_forward));
}

void OpenInBrowser(const std::string& url) {
  if (!laufey_common::IsAllowedWebViewUrl(url))
    return;
  if (laufey_common::ExternalOpenCapture::Instance().Capture(url))
    return;
  std::wstring wurl = Wide(url);
  ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL);
}

// A popup / new-tab request under the view's popup policy.
void HandlePopup(ChildView* v, const std::string& url, bool popup) {
  switch (v->options.popups) {
    case laufey_common::WebViewPopups::kSameView:
      if (v->webview && laufey_common::IsAllowedWebViewUrl(url)) {
        std::wstring wurl = Wide(url);
        v->webview->Navigate(wurl.c_str());
      }
      break;
    case laufey_common::WebViewPopups::kBrowser:
      OpenInBrowser(url);
      break;
    case laufey_common::WebViewPopups::kDeny:
      Emit(v->id, LAUFEY_WEBVIEW_EVENT_NEW_WINDOW,
           laufey_common::WebViewNewWindowJson(
               url.empty() ? "about:blank" : url, popup));
      break;
  }
}

// COREWEBVIEW2_WEB_ERROR_STATUS to the engine-neutral codes; nullptr: not a
// failure (a stop, a navigation replaced by another or refused).
const char* LoadErrorCode(COREWEBVIEW2_WEB_ERROR_STATUS status) {
  switch (status) {
    case COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED:
      return nullptr;
    case COREWEBVIEW2_WEB_ERROR_STATUS_HOST_NAME_NOT_RESOLVED:
      return "dns";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_COMMON_NAME_IS_INCORRECT:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_EXPIRED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CLIENT_CERTIFICATE_CONTAINS_ERRORS:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_REVOKED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_IS_INVALID:
      return "tls";
    case COREWEBVIEW2_WEB_ERROR_STATUS_SERVER_UNREACHABLE:
    case COREWEBVIEW2_WEB_ERROR_STATUS_TIMEOUT:
    case COREWEBVIEW2_WEB_ERROR_STATUS_ERROR_HTTP_INVALID_SERVER_RESPONSE:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_RESET:
    case COREWEBVIEW2_WEB_ERROR_STATUS_DISCONNECTED:
    case COREWEBVIEW2_WEB_ERROR_STATUS_CANNOT_CONNECT:
      return "network";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED:
      return "aborted";
    default:
      return "failed";
  }
}

// WebView2 gives a status, no text: the message of a failed load.
const char* LoadErrorMessage(COREWEBVIEW2_WEB_ERROR_STATUS status) {
  switch (status) {
    case COREWEBVIEW2_WEB_ERROR_STATUS_HOST_NAME_NOT_RESOLVED:
      return "the server's name could not be resolved";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_COMMON_NAME_IS_INCORRECT:
      return "the server's certificate is not for this host";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_EXPIRED:
      return "the server's certificate has expired";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CLIENT_CERTIFICATE_CONTAINS_ERRORS:
      return "the client certificate contains errors";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_REVOKED:
      return "the server's certificate has been revoked";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_IS_INVALID:
      return "the server's certificate is not valid";
    case COREWEBVIEW2_WEB_ERROR_STATUS_SERVER_UNREACHABLE:
      return "the server could not be reached";
    case COREWEBVIEW2_WEB_ERROR_STATUS_TIMEOUT:
      return "the connection timed out";
    case COREWEBVIEW2_WEB_ERROR_STATUS_ERROR_HTTP_INVALID_SERVER_RESPONSE:
      return "the server's response was not valid";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_ABORTED:
      return "the connection was aborted";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CONNECTION_RESET:
      return "the connection was reset";
    case COREWEBVIEW2_WEB_ERROR_STATUS_DISCONNECTED:
      return "the network connection was lost";
    case COREWEBVIEW2_WEB_ERROR_STATUS_CANNOT_CONNECT:
      return "the server could not be connected to";
    case COREWEBVIEW2_WEB_ERROR_STATUS_REDIRECT_FAILED:
      return "the server's redirect failed";
    case COREWEBVIEW2_WEB_ERROR_STATUS_VALID_AUTHENTICATION_CREDENTIALS_REQUIRED:
      return "the server requires authentication";
    case COREWEBVIEW2_WEB_ERROR_STATUS_VALID_PROXY_AUTHENTICATION_REQUIRED:
      return "the proxy requires authentication";
    default:
      return "the page could not be loaded";
  }
}

// The held-key poll's timer id on the view's host window.
constexpr UINT_PTR kHeldKeyTimer = 0x4c46;
constexpr UINT kHeldKeyPollMs = 30;

// The current modifiers, as a keyup's fields.
void FillModifiers(laufey_common::WebViewKeyFields* f) {
  f->shift_key = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  f->ctrl_key = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  f->alt_key = (GetKeyState(VK_MENU) & 0x8000) != 0;
  f->meta_key = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
}

// The keyup of held key `code`: true when it was held (and is sent).
bool SendKeyUp(ChildView* v, const std::string& code) {
  std::string key;
  if (!v->held_keys.Release(code, &key))
    return false;
  v->held_vk.erase(code);
  if (v->held_keys.empty() && v->host)
    KillTimer(v->host, kHeldKeyTimer);
  laufey_common::WebViewKeyFields f;
  f.type = "keyup";
  f.key = key;
  f.code = code;
  FillModifiers(&f);
  f.forwarded = true;
  Emit(v->id, LAUFEY_WEBVIEW_EVENT_KEY, laufey_common::WebViewKeyJson(f));
  return true;
}

// Sends the keyup of every forwarded key `v` still holds.
void ReleaseHeldKeys(ChildView* v) {
  v->held_vk.clear();
  if (v->host)
    KillTimer(v->host, kHeldKeyTimer);
  for (const auto& f : v->held_keys.ReleaseAll())
    Emit(v->id, LAUFEY_WEBVIEW_EVENT_KEY, laufey_common::WebViewKeyJson(f));
}

void NoteFocus(uint32_t id, bool focused) {
  ChildView* v = Find(id);
  if (!v || v->focused == focused)
    return;
  v->focused = focused;
  const char* reason = focused ? nullptr : v->focus_reason;
  if (!focused)
    ReleaseHeldKeys(v);
  Emit(id, LAUFEY_WEBVIEW_EVENT_FOCUS,
       laufey_common::WebViewFocusJson(focused, reason));
}

bool ConsoleAllowed(ChildView* v) {
  int64_t second = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  if (second != v->console_second) {
    v->console_second = second;
    v->console_count = 0;
  }
  return ++v->console_count <= kConsolePerSecond;
}

void EmitConsole(ChildView* v, const char* level, std::string text,
                 const std::string& source, int line) {
  if (text.size() > kConsoleMaxBytes)
    text.resize(kConsoleMaxBytes);
  Emit(v->id, LAUFEY_WEBVIEW_EVENT_CONSOLE,
       laufey_common::WebViewConsoleJson(level, text, source, line));
}

const char* ConsoleLevel(const std::string& type) {
  if (type == "warning")
    return "warn";
  if (type == "error" || type == "assert")
    return "error";
  if (type == "debug")
    return "debug";
  if (type == "info")
    return "info";
  return "log";
}

// One Runtime.RemoteObject of a console call as text.
std::string RemoteObjectText(const laufey::ValuePtr& arg) {
  laufey::ValuePtr value = Field(arg, "value");
  if (value)
    return value->IsString() ? value->GetString() : json::Serialize(value);
  if (HasString(arg, "unserializableValue"))
    return StringField(arg, "unserializableValue");
  if (HasString(arg, "description"))
    return StringField(arg, "description");
  return StringField(arg, "type");
}

// Runtime.consoleAPICalled's parameters.
void OnConsoleApiCalled(uint32_t id, const laufey::ValuePtr& params) {
  ChildView* v = Find(id);
  if (!v || !ConsoleAllowed(v))
    return;
  std::string text;
  laufey::ValuePtr args = Field(params, "args");
  if (args && args->IsList()) {
    const laufey::ValueList& list = args->GetList();
    for (size_t i = 0; i < list.size() && text.size() <= kConsoleMaxBytes;
         ++i) {
      if (i > 0)
        text += ' ';
      text += RemoteObjectText(list[i]);
    }
  }
  std::string source;
  int line = 0;
  laufey::ValuePtr frames = Field(Field(params, "stackTrace"), "callFrames");
  if (frames && frames->IsList() && !frames->GetList().empty()) {
    const laufey::ValuePtr& top = frames->GetList()[0];
    source = StringField(top, "url");
    if (IntField(top, "lineNumber", &line))
      line += 1;  // 0-based
  }
  EmitConsole(v, ConsoleLevel(StringField(params, "type")), std::move(text),
              source, line);
}

// Runtime.exceptionThrown's parameters: an uncaught error, level "error".
void OnExceptionThrown(uint32_t id, const laufey::ValuePtr& params) {
  ChildView* v = Find(id);
  if (!v || !ConsoleAllowed(v))
    return;
  laufey::ValuePtr details = Field(params, "exceptionDetails");
  int line = 0;
  if (IntField(details, "lineNumber", &line))
    line += 1;  // 0-based
  EmitConsole(v, "error", ExceptionText(details), StringField(details, "url"),
              line);
}

// --- DevTools ----------------------------------------------------------------

DevToolsCallback TakePending(uint32_t id, int call) {
  ChildView* v = Find(id);
  if (!v)
    return nullptr;
  auto it = v->pending.find(call);
  if (it == v->pending.end())
    return nullptr;
  DevToolsCallback cb = std::move(it->second);
  v->pending.erase(it);
  return cb;
}

void FailPending(ChildView* v, const char* message) {
  std::map<int, DevToolsCallback> pending;
  pending.swap(v->pending);
  for (auto& kv : pending)
    kv.second(false, ErrorDict(message));
}

void CALLBACK OnDevToolsTimeout(HWND /*hwnd*/, UINT /*msg*/, UINT_PTR timer,
                                DWORD /*time*/) {
  KillTimer(nullptr, timer);
  auto it = Timers().find(timer);
  if (it == Timers().end())
    return;
  std::pair<uint32_t, int> call = it->second;
  Timers().erase(it);
  DevToolsCallback cb = TakePending(call.first, call.second);
  if (cb)
    cb(false, ErrorDict("the page did not answer in time"));
}

// Sends a DevTools method (`params` a JSON object); `callback` runs exactly
// once (UI thread): with the result, the protocol's error, or a timeout /
// closed error.
void SendDevTools(uint32_t id, const char* method, const std::string& params,
                  DevToolsCallback callback) {
  ChildView* v = Find(id);
  if (!v || !v->webview) {
    callback(false,
             ErrorDict(v ? "the page is not ready" : "the web view is closed"));
    return;
  }
  int call = v->next_call++;
  v->pending[call] = std::move(callback);
  std::wstring wmethod = Wide(method);
  std::wstring wparams = Wide(params);
  HRESULT hr = v->webview->CallDevToolsProtocolMethod(
      wmethod.c_str(), wparams.c_str(),
      Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
          [id, call](HRESULT error, LPCWSTR result) -> HRESULT {
            DevToolsCallback cb = TakePending(id, call);
            if (!cb)
              return S_OK;  // timed out, or the view closed
            laufey::ValuePtr dict;
            if (result)
              dict = json::ParseJson(Utf8(result));
            if (!dict || !dict->IsDict())
              dict = laufey::Value::Dict();
            cb(SUCCEEDED(error), dict);
            return S_OK;
          })
          .Get());
  if (FAILED(hr)) {
    DevToolsCallback cb = TakePending(id, call);
    if (cb)
      cb(false, ErrorDict("the DevTools method could not be sent"));
    return;
  }
  UINT_PTR timer = SetTimer(nullptr, 0, kDevToolsTimeoutMs, &OnDevToolsTimeout);
  if (timer)
    Timers()[timer] = {id, call};
}

// Evaluate's last step: the result serialized in the script's world.
void EvalSerialize(uint32_t id, int context_id, const laufey::ValuePtr& remote,
                   std::function<void(int32_t, std::string)> done) {
  std::string arg = "{}";  // undefined
  if (HasString(remote, "objectId")) {
    arg = "{\"objectId\":" + Quote(StringField(remote, "objectId")) + "}";
  } else if (HasString(remote, "unserializableValue")) {
    arg = "{\"unserializableValue\":" +
          Quote(StringField(remote, "unserializableValue")) + "}";
  } else if (laufey::ValuePtr value = Field(remote, "value")) {
    arg = "{\"value\":" + json::Serialize(value) + "}";
  }
  std::string params =
      "{\"functionDeclaration\":" +
      Quote(laufey_common::kWebViewSerializeFunction) +
      ",\"executionContextId\":" + std::to_string(context_id) +
      ",\"arguments\":[" + arg +
      "],\"returnByValue\":true,\"objectGroup\":" + Quote(kEvalGroup) + "}";
  SendDevTools(id, "Runtime.callFunctionOn", params,
               [id, done](bool success, laufey::ValuePtr r) {
                 SendDevTools(id, "Runtime.releaseObjectGroup",
                              "{\"objectGroup\":" + Quote(kEvalGroup) + "}",
                              [](bool, laufey::ValuePtr) {});
                 if (!success) {
                   done(
                       Find(id) ? LAUFEY_WEBVIEW_FAILED : LAUFEY_WEBVIEW_CLOSED,
                       ErrorMessage(r, "the result could not be read"));
                   return;
                 }
                 laufey::ValuePtr details = Field(r, "exceptionDetails");
                 if (details && details->IsDict()) {
                   std::string text = ExceptionText(details);
                   const std::string prefix = "TypeError: not_serializable: ";
                   if (text.rfind(prefix, 0) == 0)
                     text = text.substr(prefix.size());
                   done(LAUFEY_WEBVIEW_NOT_SERIALIZABLE, text);
                   return;
                 }
                 laufey::ValuePtr result = Field(r, "result");
                 if (!HasString(result, "value")) {
                   done(LAUFEY_WEBVIEW_FAILED, "the result could not be read");
                   return;
                 }
                 done(LAUFEY_WEBVIEW_OK, StringField(result, "value"));
               });
}

// --- Layout ------------------------------------------------------------------

LRESULT CALLBACK HostWindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
  // A button pressed in the web view's windows: the view takes the focus
  // with the press, so FOCUS goes out before the page has the click.
  if (msg == WM_PARENTNOTIFY) {
    UINT event = LOWORD(wParam);
    if (event == WM_LBUTTONDOWN || event == WM_MBUTTONDOWN ||
        event == WM_RBUTTONDOWN || event == WM_XBUTTONDOWN) {
      for (auto& kv : Views()) {
        if (kv.second->host == hwnd) {
          NoteFocus(kv.first, true);
          break;
        }
      }
    }
  }
  // A held forwarded key released while WebView2 raised no KEY_UP for it.
  if (msg == WM_TIMER && wParam == kHeldKeyTimer) {
    for (auto& kv : Views()) {
      ChildView* v = kv.second.get();
      if (v->host != hwnd)
        continue;
      std::vector<std::string> up;
      for (const auto& held : v->held_vk) {
        if (!(GetAsyncKeyState(static_cast<int>(held.second)) & 0x8000))
          up.push_back(held.first);
      }
      for (const std::string& code : up)
        SendKeyUp(v, code);
      if (v->held_keys.empty())
        KillTimer(hwnd, kHeldKeyTimer);
      return 0;
    }
    KillTimer(hwnd, kHeldKeyTimer);
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RegisterHostClass() {
  static bool registered = false;
  if (registered)
    return true;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = HostWindowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  // White until the view's first frame, as an empty web view paints.
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
  wc.lpszClassName = kHostClass;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return false;
  registered = true;
  return true;
}

// CSS pixels to the parent's physical pixels: the page's rasterization scale
// (monitor scale and text scaling), else the window's DPI.
double ScaleFor(ChildView* v) {
  ComPtr<ICoreWebView2Controller3> page3;
  double scale = 0;
  if (v->page && SUCCEEDED(v->page.As(&page3)) && page3 &&
      SUCCEEDED(page3->get_RasterizationScale(&scale)) && scale > 0)
    return scale;
  UINT dpi = v->parent ? GetDpiForWindow(v->parent) : 0;
  return dpi ? dpi / 96.0 : 1.0;
}

// Places the host (and the controller inside it) at the view's bounds,
// relative to where the page's controller sits in the parent's client area.
void ApplyBounds(ChildView* v) {
  if (!v->host)
    return;
  double scale = ScaleFor(v);
  RECT page = {0, 0, 0, 0};
  if (v->page)
    v->page->get_Bounds(&page);
  int x = page.left + static_cast<int>(std::lround(v->bounds.x * scale));
  int y = page.top + static_cast<int>(std::lround(v->bounds.y * scale));
  int width = static_cast<int>(std::lround(v->bounds.width * scale));
  int height = static_cast<int>(std::lround(v->bounds.height * scale));
  // The stacking order is set once (Start); a move keeps it.
  SetWindowPos(v->host, nullptr, x, y, width, height,
               SWP_NOZORDER | SWP_NOACTIVATE);
  if (v->controller) {
    RECT inner = {0, 0, width, height};
    v->controller->put_Bounds(inner);
  }
}

void ApplyVisible(ChildView* v) {
  if (v->host)
    ShowWindow(v->host, v->visible ? SW_SHOWNA : SW_HIDE);
  if (v->controller)
    v->controller->put_IsVisible(v->visible ? TRUE : FALSE);
}

// Gives the keyboard focus back to the window's page (the view is hidden,
// blurred or closing while it has it), in the foreground window only (a
// focus change never activates a window); FOCUS false with `reason`.
void ReturnFocus(ChildView* v, const char* reason = nullptr) {
  if (!v->focused)
    return;
  uint32_t id = v->id;
  HWND root = v->parent ? GetAncestor(v->parent, GA_ROOT) : nullptr;
  v->focus_reason = reason;
  if (v->page && root && GetForegroundWindow() == root)
    v->page->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  // LostFocus comes later (or not at all from an inactive window): report
  // the change now, with its reason.
  if (ChildView* still = Find(id)) {
    if (still->focused)
      NoteFocus(id, false);
    if (ChildView* again = Find(id))
      again->focus_reason = nullptr;
  }
}

// --- Partitions --------------------------------------------------------------

// The WebView2 profile of a partition. Profile names are 1..64 characters of
// a limited set, compared without case, so a name that is short enough and
// lower case is used as it is, prefixed "wv~"; any other is a hash of the
// partition, prefixed "wvh~". Neither can be the app's own profile.
std::string ProfileNameFor(const std::string& partition) {
  std::string dir = laufey_common::PartitionDirName(partition);
  bool plain = !dir.empty() && dir.size() + 3 <= 64 && dir.back() != '.';
  for (char c : dir) {
    bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '-' || c == '_' || c == '~';
    if (!ok)
      plain = false;
  }
  if (plain)
    return "wv~" + dir;
  // FNV-1a, 64 bits, of the partition name (case kept).
  uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : partition) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  char hex[17];
  snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
  return std::string("wvh~") + hex;
}

// --- Creation ----------------------------------------------------------------

void SubscribeConsole(ChildView* v) {
  uint32_t id = v->id;
  if (SUCCEEDED(v->webview->GetDevToolsProtocolEventReceiver(
          L"Runtime.consoleAPICalled", &v->console_receiver)) &&
      v->console_receiver) {
    v->console_receiver->add_DevToolsProtocolEventReceived(
        Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
            [id](ICoreWebView2*,
                 ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args)
                -> HRESULT {
              LPWSTR raw = nullptr;
              if (FAILED(args->get_ParameterObjectAsJson(&raw)) || !raw)
                return S_OK;
              std::string params = Utf8(raw);
              CoTaskMemFree(raw);
              OnConsoleApiCalled(id, json::ParseJson(params));
              return S_OK;
            })
            .Get(),
        &v->console_token);
  }
  if (SUCCEEDED(v->webview->GetDevToolsProtocolEventReceiver(
          L"Runtime.exceptionThrown", &v->exception_receiver)) &&
      v->exception_receiver) {
    v->exception_receiver->add_DevToolsProtocolEventReceived(
        Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
            [id](ICoreWebView2*,
                 ICoreWebView2DevToolsProtocolEventReceivedEventArgs* args)
                -> HRESULT {
              LPWSTR raw = nullptr;
              if (FAILED(args->get_ParameterObjectAsJson(&raw)) || !raw)
                return S_OK;
              std::string params = Utf8(raw);
              CoTaskMemFree(raw);
              OnExceptionThrown(id, json::ParseJson(params));
              return S_OK;
            })
            .Get(),
        &v->exception_token);
  }
}

// The controller's AcceleratorKeyPressed: the app menu, then "forwardKeys".
void OnAcceleratorKey(uint32_t id,
                      ICoreWebView2AcceleratorKeyPressedEventArgs* args) {
  ChildView* v = Find(id);
  COREWEBVIEW2_KEY_EVENT_KIND kind;
  UINT key = 0;
  if (!v || FAILED(args->get_KeyEventKind(&kind)) ||
      FAILED(args->get_VirtualKey(&key)))
    return;
  COREWEBVIEW2_PHYSICAL_KEY_STATUS status = {};
  if (kind == COREWEBVIEW2_KEY_EVENT_KIND_KEY_UP ||
      kind == COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_UP) {
    args->get_PhysicalKeyStatus(&status);
    if (SendKeyUp(v, laufey_common::VkToCode(static_cast<int>(key),
                                             status.IsExtendedKey != FALSE,
                                             status.ScanCode)))
      args->put_Handled(TRUE);
    return;
  }
  if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
      kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
    return;
  args->get_PhysicalKeyStatus(&status);
  laufey_common::WebViewKeyFields f;
  f.shift_key = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
  f.ctrl_key = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
  f.alt_key = (GetKeyState(VK_MENU) & 0x8000) != 0;
  f.meta_key = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
  f.key = laufey_common::VkToKey(static_cast<int>(key), 0, f.shift_key,
                                 (GetKeyState(VK_CAPITAL) & 1) != 0);
  f.code = laufey_common::VkToCode(
      static_cast<int>(key), status.IsExtendedKey != FALSE, status.ScanCode);
  f.repeat = status.WasKeyDown != FALSE;
  f.forwarded = true;
  // '!' entries before the menu, the others after it.
  if (!laufey_common::WebViewKeyMatchesBeforeMenu(v->options.forward_keys,
                                                  f.code, f.alt_key, f.ctrl_key,
                                                  f.meta_key, f.shift_key)) {
    if (win32_menu::HandleAcceleratorKey(GetAncestor(v->parent, GA_ROOT),
                                         key)) {
      args->put_Handled(TRUE);
      return;
    }
    v = Find(id);
    if (!v || !laufey_common::WebViewKeyMatches(v->options.forward_keys, f.code,
                                                f.alt_key, f.ctrl_key,
                                                f.meta_key, f.shift_key))
      return;
  }
  args->put_Handled(TRUE);
  v->held_keys.Press(f.code, f.key);
  if (!f.code.empty()) {
    v->held_vk[f.code] = key;
    if (v->host)
      SetTimer(v->host, kHeldKeyTimer, kHeldKeyPollMs, nullptr);
  }
  Emit(id, LAUFEY_WEBVIEW_EVENT_KEY, laufey_common::WebViewKeyJson(f));
}

// --- Downloads the app decides (API 59) --------------------------------------

using laufey_common::WebViewDownloadPhase;

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The file name of a Windows path (after its last separator).
std::string FileNameOf(LPCWSTR path) {
  if (!path)
    return std::string();
  std::wstring p(path);
  size_t sep = p.find_last_of(L"\\/");
  return Utf8(sep == std::wstring::npos ? p.c_str() : p.c_str() + sep + 1);
}

ChildDownload* FindDownload(ChildView* v, uint32_t download_id) {
  auto it = v->downloads.find(download_id);
  return it == v->downloads.end() ? nullptr : it->second.get();
}

// Takes the download out of its view (its id is forgotten).
std::unique_ptr<ChildDownload> TakeDownload(ChildView* v,
                                            uint32_t download_id) {
  auto it = v->downloads.find(download_id);
  if (it == v->downloads.end())
    return nullptr;
  std::unique_ptr<ChildDownload> d = std::move(it->second);
  v->downloads.erase(it);
  return d;
}

bool Decided(const ChildDownload& d) {
  return d.phase != WebViewDownloadPhase::kRequested &&
         d.phase != WebViewDownloadPhase::kChoosing;
}

int CanResume(const ChildDownload& d) {
  BOOL can = FALSE;
  if (!d.operation || FAILED(d.operation->get_CanResume(&can)))
    return -1;
  return can ? 1 : 0;
}

// The DOWNLOAD event's fields for `state` (receivedBytes from the operation
// once the download is decided).
laufey_common::WebViewDownloadFields DownloadFields(const ChildDownload& d,
                                                    const char* state) {
  laufey_common::WebViewDownloadFields f;
  f.id = d.id;
  f.state = state;
  f.url = d.url;
  f.filename = d.filename;
  f.mime_type = d.mime_type;
  f.total_bytes = d.total_bytes;
  f.path = d.path;
  INT64 received = 0;
  if (Decided(d) && d.operation &&
      SUCCEEDED(d.operation->get_BytesReceived(&received)))
    f.received_bytes = received;
  if (d.operation && f.total_bytes < 0) {
    INT64 total = 0;
    if (SUCCEEDED(d.operation->get_TotalBytesToReceive(&total)) && total > 0)
      f.total_bytes = total;
  }
  return f;
}

void EmitDownload(uint32_t view_id,
                  const laufey_common::WebViewDownloadFields& f) {
  Emit(view_id, LAUFEY_WEBVIEW_EVENT_DOWNLOAD,
       laufey_common::WebViewDownloadJson(f));
}

void Unsubscribe(ChildDownload* d) {
  if (d->subscribed && d->operation) {
    d->operation->remove_BytesReceivedChanged(d->bytes_token);
    d->operation->remove_StateChanged(d->state_token);
  }
  d->subscribed = false;
}

// Ends a download that is not done, without an event: refuses the held
// DownloadStarting (nothing was written), or cancels the operation (WebView2
// removes its partial file).
void AbortDownload(ChildDownload* d) {
  Unsubscribe(d);
  if (d->deferral) {
    if (d->args)
      d->args->put_Cancel(TRUE);
    d->deferral->Complete();
    d->deferral.Reset();
    d->args.Reset();
  } else if (d->operation) {
    d->operation->Cancel();
  }
  d->phase = WebViewDownloadPhase::kDone;
}

// Cancels a download of `v` and sends its "cancelled" (its last event).
void CancelDownload(ChildView* v, uint32_t download_id) {
  uint32_t view_id = v->id;
  std::unique_ptr<ChildDownload> d = TakeDownload(v, download_id);
  if (!d)
    return;
  laufey_common::WebViewDownloadFields f =
      DownloadFields(*d, laufey_common::kWebViewDownloadCancelled);
  AbortDownload(d.get());
  EmitDownload(view_id, f);
}

// COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON to the "interrupted" reasons.
const char* InterruptReason(COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason) {
  switch (reason) {
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_FAILED:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_ACCESS_DENIED:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_NO_SPACE:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_NAME_TOO_LONG:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_TOO_LARGE:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_MALICIOUS:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_TRANSIENT_ERROR:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_BLOCKED_BY_POLICY:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_SECURITY_CHECK_FAILED:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_TOO_SHORT:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_FILE_HASH_MISMATCH:
      return laufey_common::kWebViewDownloadReasonFile;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_FAILED:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_TIMEOUT:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_DISCONNECTED:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_SERVER_DOWN:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NETWORK_INVALID_REQUEST:
      return laufey_common::kWebViewDownloadReasonNetwork;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_FAILED:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_NO_RANGE:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_BAD_CONTENT:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_UNAUTHORIZED:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_CERTIFICATE_PROBLEM:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_FORBIDDEN:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_UNEXPECTED_RESPONSE:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_CONTENT_LENGTH_MISMATCH:
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_SERVER_CROSS_ORIGIN_REDIRECT:
      return laufey_common::kWebViewDownloadReasonServer;
    case COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_DOWNLOAD_PROCESS_CRASHED:
      return laufey_common::kWebViewDownloadReasonCrash;
    default:
      return laufey_common::kWebViewDownloadReasonFailed;
  }
}

// The size of the file at `path`, or -1.
int64_t FileSize(const std::string& path) {
  WIN32_FILE_ATTRIBUTE_DATA data = {};
  std::wstring wpath = Wide(path);
  if (path.empty() ||
      !GetFileAttributesExW(wpath.c_str(), GetFileExInfoStandard, &data))
    return -1;
  ULARGE_INTEGER size;
  size.LowPart = data.nFileSizeLow;
  size.HighPart = data.nFileSizeHigh;
  return static_cast<int64_t>(size.QuadPart);
}

void ArmDownloadPoll(ChildDownload* d, int64_t now_ms);

void OnDownloadBytes(uint32_t view_id, uint32_t download_id) {
  ChildView* v = Find(view_id);
  ChildDownload* d = v ? FindDownload(v, download_id) : nullptr;
  if (!d || d->phase != WebViewDownloadPhase::kProgressing)
    return;
  int64_t now = NowMs();
  // A sign of life: the poll's grace starts again.
  ArmDownloadPoll(d, now);
  if (!d->throttle.Ready(now))
    return;
  laufey_common::WebViewDownloadFields f =
      DownloadFields(*d, laufey_common::kWebViewDownloadProgressing);
  f.can_resume = CanResume(*d);
  EmitDownload(view_id, f);
}

void OnDownloadState(uint32_t view_id, uint32_t download_id) {
  ChildView* v = Find(view_id);
  ChildDownload* d = v ? FindDownload(v, download_id) : nullptr;
  if (!d || !d->operation)
    return;
  COREWEBVIEW2_DOWNLOAD_STATE state = COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
  if (FAILED(d->operation->get_State(&state)))
    return;
  if (state == COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS) {
    d->resume_in_flight = false;
    // A resume laufey did not ask for (the phase moved with RESUME), after
    // an interruption or a pause; never while a PAUSE is still on its way
    // (this is then the state change of an earlier resume).
    if (d->phase == WebViewDownloadPhase::kInterrupted ||
        (d->phase == WebViewDownloadPhase::kPaused && !d->pause_in_flight))
      d->phase = WebViewDownloadPhase::kProgressing;
    return;
  }
  if (state == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED) {
    std::unique_ptr<ChildDownload> owned = TakeDownload(v, download_id);
    Unsubscribe(owned.get());
    LPWSTR raw = nullptr;
    if (SUCCEEDED(owned->operation->get_ResultFilePath(&raw)) && raw && *raw)
      owned->path = Utf8(raw);
    if (raw)
      CoTaskMemFree(raw);
    owned->deferral.Reset();
    owned->args.Reset();
    laufey_common::WebViewDownloadFields f =
        DownloadFields(*owned, laufey_common::kWebViewDownloadCompleted);
    int64_t size = FileSize(owned->path);
    if (size >= 0)
      f.received_bytes = size;
    owned->phase = WebViewDownloadPhase::kDone;
    EmitDownload(view_id, f);
    return;
  }
  // INTERRUPTED.
  COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason =
      COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NONE;
  d->operation->get_InterruptReason(&reason);
  if (reason == COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_PAUSED) {
    // WebView2's pause. PAUSE already reported it; a pause laufey did not
    // ask for is reported now. A RESUME that came before the pause took
    // effect (its Resume() would have done nothing) is applied now; a
    // Resume() on its way makes this the old pause's state.
    if (d->resume_in_flight && !d->pause_in_flight)
      return;
    d->pause_in_flight = false;
    if (d->resume_pending) {
      d->resume_pending = false;
      if (SUCCEEDED(d->operation->Resume())) {
        d->resume_in_flight = true;
        d->phase = WebViewDownloadPhase::kProgressing;
        return;
      }
      // It stays paused: report it.
      d->phase = WebViewDownloadPhase::kProgressing;
    }
    if (d->phase == WebViewDownloadPhase::kProgressing) {
      d->phase = WebViewDownloadPhase::kPaused;
      laufey_common::WebViewDownloadFields f =
          DownloadFields(*d, laufey_common::kWebViewDownloadProgressing);
      f.paused = true;
      f.can_resume = CanResume(*d);
      EmitDownload(view_id, f);
    }
    return;
  }
  if (reason == COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_CANCELED) {
    CancelDownload(v, download_id);
    return;
  }
  // Reported already ("interrupted" with "canResume": true) and not resumed
  // since: the same interruption seen again (by the poll, then by its
  // StateChanged), never sent twice.
  if (d->phase == WebViewDownloadPhase::kInterrupted)
    return;
  int can_resume = Decided(*d) ? CanResume(*d) : 0;
  laufey_common::WebViewDownloadFields f =
      DownloadFields(*d, laufey_common::kWebViewDownloadInterrupted);
  f.reason = InterruptReason(reason);
  f.can_resume = can_resume > 0 ? 1 : 0;
  if (can_resume > 0) {
    d->phase = WebViewDownloadPhase::kInterrupted;
    EmitDownload(view_id, f);
    return;
  }
  std::unique_ptr<ChildDownload> owned = TakeDownload(v, download_id);
  // A download interrupted while it waited still holds its event.
  AbortDownload(owned.get());
  EmitDownload(view_id, f);
}

void Subscribe(uint32_t view_id, ChildDownload* d) {
  if (!d->operation || d->subscribed)
    return;
  uint32_t download_id = d->id;
  HRESULT bytes = d->operation->add_BytesReceivedChanged(
      Callback<ICoreWebView2BytesReceivedChangedEventHandler>(
          [view_id, download_id](ICoreWebView2DownloadOperation*,
                                 IUnknown*) -> HRESULT {
            OnDownloadBytes(view_id, download_id);
            return S_OK;
          })
          .Get(),
      &d->bytes_token);
  HRESULT state = d->operation->add_StateChanged(
      Callback<ICoreWebView2StateChangedEventHandler>(
          [view_id, download_id](ICoreWebView2DownloadOperation*,
                                 IUnknown*) -> HRESULT {
            OnDownloadState(view_id, download_id);
            return S_OK;
          })
          .Get(),
      &d->state_token);
  d->subscribed = SUCCEEDED(bytes) && SUCCEEDED(state);
}

// WebView2 has been seen to raise a download's StateChanged 20+ s late, or
// not at all, on a loaded machine (the completion the app waits on never
// came). So while a decided download may still end, the UI thread reads its
// state every kWebViewDownloadPollIntervalMs (one thread timer for every
// view) and hands a completion, or an interruption StateChanged has not
// reported, to OnDownloadState: its event goes once whichever comes first.
// Each download's poll ends kWebViewDownloadPollGraceMs after its last sign
// of life; the timer stops when no download polls. UI thread only.
UINT_PTR g_download_poll_timer = 0;

void CALLBACK OnDownloadPoll(HWND /*hwnd*/, UINT /*msg*/, UINT_PTR /*timer*/,
                             DWORD /*time*/) {
  int64_t now = NowMs();
  std::vector<std::pair<uint32_t, uint32_t>> due;
  for (auto& kv : Views()) {
    for (auto& dk : kv.second->downloads) {
      if (dk.second->poll.Due(now))
        due.emplace_back(kv.first, dk.first);
    }
  }
  if (due.empty()) {
    if (g_download_poll_timer)
      KillTimer(nullptr, g_download_poll_timer);
    g_download_poll_timer = 0;
    return;
  }
  for (const auto& [view_id, download_id] : due) {
    ChildView* v = Find(view_id);
    ChildDownload* d = v ? FindDownload(v, download_id) : nullptr;
    if (!d || !d->operation)
      continue;  // ended by an earlier one's event handler
    COREWEBVIEW2_DOWNLOAD_STATE state = COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
    if (FAILED(d->operation->get_State(&state)))
      continue;
    COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON reason =
        COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_NONE;
    laufey_common::WebViewEngineDownloadState read =
        laufey_common::WebViewEngineDownloadState::kInProgress;
    if (state == COREWEBVIEW2_DOWNLOAD_STATE_COMPLETED) {
      read = laufey_common::WebViewEngineDownloadState::kCompleted;
    } else if (state == COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED) {
      read = laufey_common::WebViewEngineDownloadState::kInterrupted;
      d->operation->get_InterruptReason(&reason);
    }
    if (!laufey_common::WebViewDownloadPollReports(
            d->phase, read,
            reason == COREWEBVIEW2_DOWNLOAD_INTERRUPT_REASON_USER_PAUSED,
            d->pause_in_flight))
      continue;
    fprintf(stderr,
            "laufey: web view %u: download %u %s with no StateChanged from "
            "WebView2 yet; reported from its state poll\n",
            view_id, download_id,
            read == laufey_common::WebViewEngineDownloadState::kCompleted
                ? "completed"
                : "was interrupted");
    OnDownloadState(view_id, download_id);
  }
}

// Arms (or extends) `d`'s poll and starts the timer if it is not running.
void ArmDownloadPoll(ChildDownload* d, int64_t now_ms) {
  d->poll.Touch(now_ms);
  if (!g_download_poll_timer)
    g_download_poll_timer =
        SetTimer(nullptr, 0, laufey_common::kWebViewDownloadPollIntervalMs,
                 &OnDownloadPoll);
}

// A DownloadStarting of a view under "downloads": "app": held open with its
// deferral, and DOWNLOAD "requested".
void OnAppDownloadStarting(ChildView* v,
                           ICoreWebView2DownloadStartingEventArgs* args) {
  // WebView2's own download UI never shows.
  args->put_Handled(TRUE);
  size_t waiting = 0;
  for (const auto& kv : v->downloads) {
    if (!Decided(*kv.second))
      ++waiting;
  }
  if (waiting >= static_cast<size_t>(LAUFEY_WEBVIEW_MAX_PENDING_DOWNLOADS)) {
    fprintf(stderr,
            "laufey: web view %u: %d downloads already wait for an answer; "
            "a new one is cancelled\n",
            v->id, LAUFEY_WEBVIEW_MAX_PENDING_DOWNLOADS);
    args->put_Cancel(TRUE);
    return;
  }
  ComPtr<ICoreWebView2Deferral> deferral;
  if (FAILED(args->GetDeferral(&deferral)) || !deferral) {
    args->put_Cancel(TRUE);
    return;
  }
  auto d = std::make_unique<ChildDownload>();
  d->id = laufey_common::NextWebViewDownloadId();
  d->args = args;
  d->deferral = deferral;
  args->get_DownloadOperation(&d->operation);
  if (d->operation) {
    LPWSTR raw = nullptr;
    if (SUCCEEDED(d->operation->get_Uri(&raw)) && raw)
      d->url = Utf8(raw);
    if (raw)
      CoTaskMemFree(raw);
    raw = nullptr;
    if (SUCCEEDED(d->operation->get_MimeType(&raw)) && raw)
      d->mime_type = Utf8(raw);
    if (raw)
      CoTaskMemFree(raw);
    INT64 total = 0;
    if (SUCCEEDED(d->operation->get_TotalBytesToReceive(&total)) && total > 0)
      d->total_bytes = total;
  }
  // ResultFilePath is pre-filled with the suggested name in the default
  // download folder; only the name is the suggestion.
  LPWSTR suggested = nullptr;
  if (SUCCEEDED(args->get_ResultFilePath(&suggested)) && suggested)
    d->filename =
        laufey_common::SanitizeDownloadFileName(FileNameOf(suggested));
  if (suggested)
    CoTaskMemFree(suggested);
  uint32_t view_id = v->id;
  uint32_t download_id = d->id;
  // Before the answer too: an engine-side end while it waits is reported.
  Subscribe(view_id, d.get());
  laufey_common::WebViewDownloadFields f =
      DownloadFields(*d, laufey_common::kWebViewDownloadRequested);
  v->downloads[download_id] = std::move(d);
  EmitDownload(view_id, f);
}

// SAVE (or the save dialog's answer): the held event writes to `path`.
// Sends the first "progressing" (with the path).
int32_t ApplySave(ChildView* v, ChildDownload* d, const std::string& path) {
  if (!d->args || !d->deferral)
    return LAUFEY_WEBVIEW_FAILED;
  std::wstring wpath = Wide(path);
  // An existing file there is replaced (the app, or the save dialog, asked):
  // removed first, so WebView2 never saves beside it under another name and
  // nothing of it stays should the download not complete.
  if (!DeleteFileW(wpath.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
    return LAUFEY_WEBVIEW_FAILED;
  if (FAILED(d->args->put_ResultFilePath(wpath.c_str())))
    return LAUFEY_WEBVIEW_FAILED;
  d->path = path;
  d->phase = WebViewDownloadPhase::kProgressing;
  ComPtr<ICoreWebView2Deferral> deferral = std::move(d->deferral);
  d->args.Reset();
  uint32_t view_id = v->id;
  int64_t now = NowMs();
  d->throttle.Ready(now);
  // Its end is read from the operation too, should StateChanged not come.
  ArmDownloadPoll(d, now);
  laufey_common::WebViewDownloadFields f =
      DownloadFields(*d, laufey_common::kWebViewDownloadProgressing);
  f.can_resume = CanResume(*d);
  deferral->Complete();
  EmitDownload(view_id, f);
  return LAUFEY_WEBVIEW_OK;
}

// SAVE_AS: the save dialog answered (UI thread); empty: dismissed.
void OnSaveAsChosen(uint32_t view_id, uint32_t download_id,
                    const std::string& chosen) {
  ChildView* v = Find(view_id);
  ChildDownload* d = v ? FindDownload(v, download_id) : nullptr;
  if (!d || d->phase != WebViewDownloadPhase::kChoosing)
    return;  // cancelled meanwhile, or the view closed
  if (chosen.empty()) {
    CancelDownload(v, download_id);
    return;
  }
  if (ApplySave(v, d, chosen) != LAUFEY_WEBVIEW_OK) {
    if (ChildView* still = Find(view_id))
      CancelDownload(still, download_id);
  }
}

void CancelPermissions(ChildView* v);

// Ends every download and permission request of a closing view, each with
// its "cancelled" (before CLOSED).
void EndDownloadsAndPermissions(ChildView* v) {
  uint32_t view_id = v->id;
  std::map<uint32_t, std::unique_ptr<ChildDownload>> downloads;
  downloads.swap(v->downloads);
  for (auto& kv : downloads) {
    laufey_common::WebViewDownloadFields f =
        DownloadFields(*kv.second, laufey_common::kWebViewDownloadCancelled);
    AbortDownload(kv.second.get());
    EmitDownload(view_id, f);
  }
  CancelPermissions(v);
}

// --- Permission requests the app answers (API 59) ----------------------------

// The PERMISSION_REQUEST name of a WebView2 permission kind, or nullptr (one
// laufey can't name: refused without an event).
const char* PermissionName(COREWEBVIEW2_PERMISSION_KIND kind) {
  switch (kind) {
    case COREWEBVIEW2_PERMISSION_KIND_MICROPHONE:
      return laufey_common::kWebViewPermissionMicrophone;
    case COREWEBVIEW2_PERMISSION_KIND_CAMERA:
      return laufey_common::kWebViewPermissionCamera;
    case COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION:
      return laufey_common::kWebViewPermissionGeolocation;
    case COREWEBVIEW2_PERMISSION_KIND_NOTIFICATIONS:
      return laufey_common::kWebViewPermissionNotifications;
    case COREWEBVIEW2_PERMISSION_KIND_OTHER_SENSORS:
      return laufey_common::kWebViewPermissionSensors;
    case COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ:
      return laufey_common::kWebViewPermissionClipboardRead;
    case COREWEBVIEW2_PERMISSION_KIND_MULTIPLE_AUTOMATIC_DOWNLOADS:
      return laufey_common::kWebViewPermissionMultipleDownloads;
    case COREWEBVIEW2_PERMISSION_KIND_FILE_READ_WRITE:
      return laufey_common::kWebViewPermissionFileSystem;
    case COREWEBVIEW2_PERMISSION_KIND_AUTOPLAY:
      return laufey_common::kWebViewPermissionAutoplay;
    case COREWEBVIEW2_PERMISSION_KIND_LOCAL_FONTS:
      return laufey_common::kWebViewPermissionLocalFonts;
    case COREWEBVIEW2_PERMISSION_KIND_MIDI_SYSTEM_EXCLUSIVE_MESSAGES:
      return laufey_common::kWebViewPermissionMidi;
    case COREWEBVIEW2_PERMISSION_KIND_WINDOW_MANAGEMENT:
      return laufey_common::kWebViewPermissionWindowManagement;
    default:
      return nullptr;
  }
}

// Sets the answer, never saved in the partition's profile where the runtime
// lets that be said (ICoreWebView2PermissionRequestedEventArgs3).
void SetPermissionState(ICoreWebView2PermissionRequestedEventArgs* args,
                        bool allow) {
  ComPtr<ICoreWebView2PermissionRequestedEventArgs3> args3;
  if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args3))) && args3)
    args3->put_SavesInProfile(FALSE);
  args->put_State(allow ? COREWEBVIEW2_PERMISSION_STATE_ALLOW
                        : COREWEBVIEW2_PERMISSION_STATE_DENY);
}

void ResolvePermission(ChildPermission* p, bool allow) {
  if (p->args)
    SetPermissionState(p->args.Get(), allow);
  if (p->deferral)
    p->deferral->Complete();
  p->deferral.Reset();
  p->args.Reset();
}

// Refuses every request `v` still waits on, each with its "cancelled".
void CancelPermissions(ChildView* v) {
  uint32_t view_id = v->id;
  std::map<uint32_t, ChildPermission> pending;
  pending.swap(v->permissions);
  for (auto& kv : pending) {
    ResolvePermission(&kv.second, false);
    Emit(view_id, LAUFEY_WEBVIEW_EVENT_PERMISSION_REQUEST,
         laufey_common::WebViewPermissionCancelledJson(kv.first));
  }
}

// A PermissionRequested of a view under "permissions": "app": held open with
// its deferral, and PERMISSION_REQUEST "requested". `frame_is_main`: 1 the
// main frame, 0 a subframe, -1 not known.
void OnAppPermissionRequested(uint32_t id,
                              ICoreWebView2PermissionRequestedEventArgs* args,
                              int frame_is_main) {
  ChildView* v = Find(id);
  COREWEBVIEW2_PERMISSION_KIND kind =
      COREWEBVIEW2_PERMISSION_KIND_UNKNOWN_PERMISSION;
  args->get_PermissionKind(&kind);
  const char* name = PermissionName(kind);
  ComPtr<ICoreWebView2Deferral> deferral;
  if (!v || !name ||
      v->permissions.size() >=
          static_cast<size_t>(LAUFEY_WEBVIEW_MAX_PENDING_PERMISSIONS) ||
      FAILED(args->GetDeferral(&deferral)) || !deferral) {
    SetPermissionState(args, false);
    return;
  }
  LPWSTR raw = nullptr;
  std::string url;
  if (SUCCEEDED(args->get_Uri(&raw)) && raw)
    url = Utf8(raw);
  if (raw)
    CoTaskMemFree(raw);
  laufey_common::WebViewPermissionFields f;
  f.id = laufey_common::NextWebViewPermissionId();
  f.origin = laufey_common::WebViewOriginOf(url);
  f.permissions.push_back(name);
  f.frame_is_main = frame_is_main;
  if (frame_is_main >= 0)
    f.frame_origin = f.origin;
  ChildPermission p;
  p.args = args;
  p.deferral = deferral;
  v->permissions[f.id] = std::move(p);
  Emit(id, LAUFEY_WEBVIEW_EVENT_PERMISSION_REQUEST,
       laufey_common::WebViewPermissionRequestJson(f));
}

// Under "permissions": "app": each subframe's PermissionRequested is handled
// on its ICoreWebView2Frame (raised there before the CoreWebView2, for the
// frame and its descendants), reported as a subframe's.
void AttributeFrames(ChildView* view, ICoreWebView2_4* webview4) {
  uint32_t id = view->id;
  EventRegistrationToken token = {};
  if (FAILED(webview4->add_FrameCreated(
          Callback<ICoreWebView2FrameCreatedEventHandler>(
              [id](ICoreWebView2*,
                   ICoreWebView2FrameCreatedEventArgs* args) -> HRESULT {
                ComPtr<ICoreWebView2Frame> frame;
                ComPtr<ICoreWebView2Frame3> frame3;
                EventRegistrationToken frame_token = {};
                bool ok =
                    SUCCEEDED(args->get_Frame(&frame)) && frame &&
                    SUCCEEDED(frame.As(&frame3)) && frame3 &&
                    SUCCEEDED(frame3->add_PermissionRequested(
                        Callback<
                            ICoreWebView2FramePermissionRequestedEventHandler>(
                            [id](ICoreWebView2Frame*,
                                 ICoreWebView2PermissionRequestedEventArgs2*
                                     request) -> HRESULT {
                              // Not raised again on the CoreWebView2.
                              request->put_Handled(TRUE);
                              OnAppPermissionRequested(id, request, 0);
                              return S_OK;
                            })
                            .Get(),
                        &frame_token));
                // A frame without the handler: its requests reach the
                // CoreWebView2's, which then can't tell the frames apart.
                if (!ok) {
                  if (ChildView* v = Find(id))
                    v->frames_attributed = false;
                }
                return S_OK;
              })
              .Get(),
          &token)))
    return;
  view->frames_attributed = true;
}

// The view's settings and event handlers, before its first navigation.
void Configure(ChildView* view) {
  uint32_t id = view->id;
  ICoreWebView2* webview = view->webview.Get();

  ComPtr<ICoreWebView2Settings> settings;
  if (SUCCEEDED(webview->get_Settings(&settings)) && settings) {
    settings->put_IsWebMessageEnabled(FALSE);
    settings->put_AreHostObjectsAllowed(FALSE);
    // DevTools follow the app's own page (LAUFEY_INSPECTABLE).
    settings->put_AreDevToolsEnabled(
        laufey_common::LaunchInspectable() ? TRUE : FALSE);
    ComPtr<ICoreWebView2Settings2> settings2;
    if (!view->options.user_agent.empty() &&
        SUCCEEDED(settings.As(&settings2)) && settings2) {
      std::wstring ua = Wide(view->options.user_agent);
      settings2->put_UserAgent(ua.c_str());
    }
    // "downloads": "app": the app decides each download and where it goes,
    // so WebView2's SmartScreen reputation check (which can hold a
    // download's completion while it answers) is off for this view
    // (ICoreWebView2Settings8; an older runtime keeps it). WebView2 has the
    // setting per web view only, so it covers the view's navigations too.
    ComPtr<ICoreWebView2Settings8> settings8;
    if (view->options.downloads == laufey_common::WebViewDownloads::kApp &&
        SUCCEEDED(settings.As(&settings8)) && settings8)
      settings8->put_IsReputationCheckingRequired(FALSE);
  }

  webview->add_NavigationStarting(
      Callback<ICoreWebView2NavigationStartingEventHandler>(
          [id](ICoreWebView2*,
               ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
            LPWSTR raw = nullptr;
            std::string url;
            if (SUCCEEDED(args->get_Uri(&raw)) && raw)
              url = Utf8(raw);
            if (raw)
              CoTaskMemFree(raw);
            ChildView* v = Find(id);
            // file:, data:, javascript:, the app's schemes, mailto: ...
            if (!v || !laufey_common::IsAllowedWebViewNavigation(url)) {
              args->put_Cancel(TRUE);
              return S_OK;
            }
            UINT64 navigation = 0;
            args->get_NavigationId(&navigation);
            BOOL redirected = FALSE;
            args->get_IsRedirected(&redirected);
            v->navigation = navigation;
            if (!redirected) {
              v->download_started = false;
              SetLoad(v, "loading", url);
              // The page goes away: its requests still waiting are refused.
              if (ChildView* still = Find(id))
                CancelPermissions(still);
            }
            return S_OK;
          })
          .Get(),
      nullptr);

  webview->add_ContentLoading(
      Callback<ICoreWebView2ContentLoadingEventHandler>(
          [id](ICoreWebView2*,
               ICoreWebView2ContentLoadingEventArgs* args) -> HRESULT {
            ChildView* v = Find(id);
            if (!v)
              return S_OK;
            BOOL error_page = FALSE;
            args->get_IsErrorPage(&error_page);
            UINT64 navigation = 0;
            args->get_NavigationId(&navigation);
            // The error page of a failed load is not the page's document.
            if (error_page || navigation != v->navigation)
              return S_OK;
            SetLoad(v, "interactive", CurrentUrl(v));
            MaybeSendUrl(v);
            return S_OK;
          })
          .Get(),
      nullptr);

  webview->add_NavigationCompleted(
      Callback<ICoreWebView2NavigationCompletedEventHandler>(
          [id](ICoreWebView2*,
               ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            ChildView* v = Find(id);
            if (!v)
              return S_OK;
            UINT64 navigation = 0;
            args->get_NavigationId(&navigation);
            if (navigation != v->navigation)
              return S_OK;  // replaced by a later navigation
            BOOL success = FALSE;
            args->get_IsSuccess(&success);
            if (success) {
              SetLoad(v, "complete", CurrentUrl(v));
              MaybeSendUrl(v);
              return S_OK;
            }
            COREWEBVIEW2_WEB_ERROR_STATUS status =
                COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
            args->get_WebErrorStatus(&status);
            const char* code = LoadErrorCode(status);
            if (!code || v->download_started) {
              // Stopped, or a download: no "failed"; the view is not loading
              // it any more.
              v->load = "complete";
              return S_OK;
            }
            SetLoad(v, "failed", CurrentUrl(v), code, LoadErrorMessage(status));
            return S_OK;
          })
          .Get(),
      nullptr);

  webview->add_DocumentTitleChanged(
      Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
          [id](ICoreWebView2* sender, IUnknown*) -> HRESULT {
            ChildView* v = Find(id);
            if (!v)
              return S_OK;
            LPWSTR raw = nullptr;
            std::string title;
            if (SUCCEEDED(sender->get_DocumentTitle(&raw)) && raw)
              title = Utf8(raw);
            if (raw)
              CoTaskMemFree(raw);
            v->title = title;
            Emit(id, LAUFEY_WEBVIEW_EVENT_TITLE,
                 laufey_common::WebViewTitleJson(v->title));
            return S_OK;
          })
          .Get(),
      nullptr);

  webview->add_SourceChanged(
      Callback<ICoreWebView2SourceChangedEventHandler>(
          [id](ICoreWebView2*,
               ICoreWebView2SourceChangedEventArgs*) -> HRESULT {
            if (ChildView* v = Find(id))
              MaybeSendUrl(v);
            return S_OK;
          })
          .Get(),
      nullptr);

  webview->add_HistoryChanged(Callback<ICoreWebView2HistoryChangedEventHandler>(
                                  [id](ICoreWebView2*, IUnknown*) -> HRESULT {
                                    if (ChildView* v = Find(id))
                                      MaybeSendUrl(v);
                                    return S_OK;
                                  })
                                  .Get(),
                              nullptr);

  // No second web view ever: Handled with no NewWindow makes the page's
  // window.open() return null, and the policy decides where the URL goes.
  webview->add_NewWindowRequested(
      Callback<ICoreWebView2NewWindowRequestedEventHandler>(
          [id](ICoreWebView2*,
               ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            args->put_Handled(TRUE);
            ChildView* v = Find(id);
            if (!v)
              return S_OK;
            LPWSTR raw = nullptr;
            std::string url;
            if (SUCCEEDED(args->get_Uri(&raw)) && raw)
              url = Utf8(raw);
            if (raw)
              CoTaskMemFree(raw);
            bool popup = false;
            ComPtr<ICoreWebView2WindowFeatures> features;
            if (SUCCEEDED(args->get_WindowFeatures(&features)) && features) {
              BOOL has_position = FALSE, has_size = FALSE;
              features->get_HasPosition(&has_position);
              features->get_HasSize(&has_size);
              popup = has_position || has_size;
            }
            HandlePopup(v, url, popup);
            return S_OK;
          })
          .Get(),
      nullptr);

  ComPtr<ICoreWebView2_4> webview4;
  if (SUCCEEDED(view->webview.As(&webview4)) && webview4) {
    webview4->add_DownloadStarting(
        Callback<ICoreWebView2DownloadStartingEventHandler>(
            [id](ICoreWebView2*,
                 ICoreWebView2DownloadStartingEventArgs* args) -> HRESULT {
              ChildView* v = Find(id);
              if (!v) {
                args->put_Cancel(TRUE);
                return S_OK;
              }
              v->download_started = true;
              switch (v->options.downloads) {
                case laufey_common::WebViewDownloads::kAsk:
                  // WebView2's own download handling and UI.
                  return S_OK;
                case laufey_common::WebViewDownloads::kBrowser: {
                  args->put_Cancel(TRUE);
                  ComPtr<ICoreWebView2DownloadOperation> download;
                  LPWSTR raw = nullptr;
                  if (SUCCEEDED(args->get_DownloadOperation(&download)) &&
                      download && SUCCEEDED(download->get_Uri(&raw)) && raw)
                    OpenInBrowser(Utf8(raw));
                  if (raw)
                    CoTaskMemFree(raw);
                  return S_OK;
                }
                case laufey_common::WebViewDownloads::kDeny:
                  args->put_Cancel(TRUE);
                  return S_OK;
                case laufey_common::WebViewDownloads::kApp:
                  OnAppDownloadStarting(v, args);
                  return S_OK;
              }
              args->put_Cancel(TRUE);
              return S_OK;
            })
            .Get(),
        nullptr);
  }

  // "ask": WebView2's own prompt (the default); "deny": refused without
  // one, and the refusal is not saved in the partition's profile; "app":
  // held for the app (OnAppPermissionRequested), the main frame's when every
  // subframe has its own handler (AttributeFrames).
  if (view->options.permissions == laufey_common::WebViewPermissions::kApp &&
      webview4)
    AttributeFrames(view, webview4.Get());
  webview->add_PermissionRequested(
      Callback<ICoreWebView2PermissionRequestedEventHandler>(
          [id](ICoreWebView2*,
               ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
            ChildView* v = Find(id);
            if (v && v->options.permissions ==
                         laufey_common::WebViewPermissions::kAsk)
              return S_OK;
            if (v && v->options.permissions ==
                         laufey_common::WebViewPermissions::kApp) {
              OnAppPermissionRequested(id, args, v->frames_attributed ? 1 : -1);
              return S_OK;
            }
            SetPermissionState(args, false);
            return S_OK;
          })
          .Get(),
      nullptr);

  // Screen capture raises no PermissionRequested (WebView2 shows its source
  // picker): under "deny" and "app" it is refused before the picker (the app
  // is never asked: no "screen" request on WebView2).
  ComPtr<ICoreWebView2_27> webview27;
  if (view->options.permissions != laufey_common::WebViewPermissions::kAsk &&
      SUCCEEDED(view->webview.As(&webview27)) && webview27) {
    webview27->add_ScreenCaptureStarting(
        Callback<ICoreWebView2ScreenCaptureStartingEventHandler>(
            [](ICoreWebView2*,
               ICoreWebView2ScreenCaptureStartingEventArgs* args) -> HRESULT {
              args->put_Cancel(TRUE);
              return S_OK;
            })
            .Get(),
        nullptr);
  }

  webview->add_ProcessFailed(
      Callback<ICoreWebView2ProcessFailedEventHandler>(
          [id](ICoreWebView2*,
               ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            ChildView* v = Find(id);
            COREWEBVIEW2_PROCESS_FAILED_KIND kind =
                COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;
            if (!v || FAILED(args->get_ProcessFailedKind(&kind)))
              return S_OK;
            if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED)
              SetLoad(v, "failed", CurrentUrl(v), "failed",
                      "the page's renderer process ended");
            else if (kind ==
                     COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED)
              SetLoad(v, "failed", CurrentUrl(v), "failed",
                      "the browser process ended");
            return S_OK;
          })
          .Get(),
      nullptr);

  view->controller->add_AcceleratorKeyPressed(
      Callback<ICoreWebView2AcceleratorKeyPressedEventHandler>(
          [id](ICoreWebView2Controller*,
               ICoreWebView2AcceleratorKeyPressedEventArgs* args) -> HRESULT {
            OnAcceleratorKey(id, args);
            return S_OK;
          })
          .Get(),
      nullptr);
  view->controller->add_GotFocus(
      Callback<ICoreWebView2FocusChangedEventHandler>(
          [id](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
            NoteFocus(id, true);
            return S_OK;
          })
          .Get(),
      nullptr);
  view->controller->add_LostFocus(
      Callback<ICoreWebView2FocusChangedEventHandler>(
          [id](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
            NoteFocus(id, false);
            return S_OK;
          })
          .Get(),
      nullptr);

  SubscribeConsole(view);
}

void StartFirstLoad(uint32_t id) {
  ChildView* v = Find(id);
  if (!v || !v->webview || v->first_load)
    return;
  v->first_load = true;
  if (!v->pending_url.empty()) {
    std::wstring url = Wide(v->pending_url);
    v->webview->Navigate(url.c_str());
  }
}

void OnControllerCreated(uint32_t id, HRESULT result,
                         ICoreWebView2Controller* controller) {
  ChildView* v = Find(id);
  if (!v) {
    // Closed while it was being created.
    if (controller)
      controller->Close();
    return;
  }
  v->creating = false;
  if (SUCCEEDED(result) && controller) {
    v->controller = controller;
    controller->get_CoreWebView2(&v->webview);
    if (!v->webview) {
      controller->Close();
      v->controller.Reset();
      result = E_FAIL;
    }
  }
  if (FAILED(result) || !v->controller) {
    char message[96];
    snprintf(message, sizeof(message),
             "the web view could not be created (hr=0x%08lx)",
             static_cast<unsigned long>(result));
    Fail(id, message);
    return;
  }
  Configure(v);
  ApplyBounds(v);
  ApplyVisible(v);
  // Console events from the first document on: the first load waits for
  // Runtime.enable (and starts whatever its answer).
  SendDevTools(id, "Runtime.enable", "{}",
               [id](bool, laufey::ValuePtr) { StartFirstLoad(id); });
}

}  // namespace

uint32_t Capabilities() {
  int known = g_env_profiles.load();
  bool supported = known == 1;
  if (known == 0) {
    // No environment yet: the installed runtime's version. Profiles
    // (ICoreWebView2Environment10) are in every runtime from 110 on.
    static const bool runtime_has_profiles = [] {
      LPWSTR version = nullptr;
      bool ok = false;
      if (SUCCEEDED(GetAvailableCoreWebView2BrowserVersionString(nullptr,
                                                                 &version)) &&
          version) {
        int result = -1;
        ok =
            SUCCEEDED(CompareBrowserVersions(version, L"110.0.0.0", &result)) &&
            result >= 0;
      }
      if (version)
        CoTaskMemFree(version);
      return ok;
    }();
    supported = runtime_has_profiles;
  }
  if (!supported)
    return 0;
  return LAUFEY_WEBVIEW_CAP_SUPPORTED | LAUFEY_WEBVIEW_CAP_CAPTURE |
         LAUFEY_WEBVIEW_CAP_EVALUATE | LAUFEY_WEBVIEW_CAP_Z_ORDER |
         // AcceleratorKeyPressed (OnAcceleratorKey).
         LAUFEY_WEBVIEW_CAP_MENU_ACCELERATORS |
         LAUFEY_WEBVIEW_CAP_FORWARD_KEYS |
         LAUFEY_WEBVIEW_CAP_FORWARD_KEYS_BEFORE_MENU |
         LAUFEY_WEBVIEW_CAP_KEY_UP | LAUFEY_WEBVIEW_CAP_BLUR |
         LAUFEY_WEBVIEW_CAP_PERSISTENT_PARTITIONS |
         LAUFEY_WEBVIEW_CAP_PERMISSION_PROMPTS |
         // DownloadStarting (ICoreWebView2_4) and PermissionRequested's
         // deferral are in every runtime with profiles (110+).
         LAUFEY_WEBVIEW_CAP_DOWNLOAD_EVENTS |
         LAUFEY_WEBVIEW_CAP_DOWNLOAD_PAUSE |
         LAUFEY_WEBVIEW_CAP_PERMISSION_REQUESTS |
         (LAUFEY_WEBVIEW_ENGINE_WEBVIEW2 << LAUFEY_WEBVIEW_ENGINE_SHIFT);
}

bool SupportsEnvironment(ICoreWebView2Environment* env) {
  ComPtr<ICoreWebView2Environment10> env10;
  return env && SUCCEEDED(env->QueryInterface(IID_PPV_ARGS(&env10))) && env10;
}

void NoteEnvironment(ICoreWebView2Environment* env) {
  if (env)
    g_env_profiles.store(SupportsEnvironment(env) ? 1 : 2);
}

uint32_t Create(uint32_t window_id, HWND parent, const WebViewOptions& options,
                laufey_webview_event_fn on_event, void* user_data,
                int32_t* status_out) {
  if (!parent || !IsWindow(parent)) {
    *status_out = LAUFEY_WEBVIEW_NO_WINDOW;
    return 0;
  }
  uint32_t id = WebViewSinks::Instance().Add(window_id, on_event, user_data);
  if (!id) {
    *status_out = LAUFEY_WEBVIEW_LIMIT;
    return 0;
  }
  HWND host = nullptr;
  if (RegisterHostClass()) {
    host = CreateWindowExW(
        0, kHostClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0,
        0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
  }
  if (!host) {
    WebViewSinks::Instance().Abandon(id);
    *status_out = LAUFEY_WEBVIEW_FAILED;
    return 0;
  }
  auto v = std::make_unique<ChildView>();
  v->id = id;
  v->window_id = window_id;
  v->parent = parent;
  v->host = host;
  v->options = options;
  v->bounds = options.bounds;
  v->visible = options.visible;
  v->pending_url = options.url;
  ChildView* raw = v.get();
  Views()[id] = std::move(v);
  SetWindowPos(host, HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  ApplyBounds(raw);
  ApplyVisible(raw);
  *status_out = LAUFEY_WEBVIEW_OK;
  return id;
}

HRESULT Start(uint32_t id, ICoreWebView2Environment* env,
              ICoreWebView2Controller* page) {
  ChildView* v = Find(id);
  if (!v || v->creating || v->controller)
    return S_OK;
  ComPtr<ICoreWebView2Environment10> env10;
  if (!env || FAILED(env->QueryInterface(IID_PPV_ARGS(&env10))) || !env10)
    return E_NOINTERFACE;
  ComPtr<ICoreWebView2ControllerOptions> options;
  HRESULT hr = env10->CreateCoreWebView2ControllerOptions(&options);
  if (FAILED(hr) || !options)
    return FAILED(hr) ? hr : E_FAIL;
  std::wstring profile = Wide(ProfileNameFor(v->options.partition));
  hr = options->put_ProfileName(profile.c_str());
  if (FAILED(hr))
    return hr;
  hr = options->put_IsInPrivateModeEnabled(
      laufey_common::IsEphemeralPartition(v->options.partition) ? TRUE : FALSE);
  if (FAILED(hr))
    return hr;
  v->page = page;
  // Above the page's controller (whose window may be newer than the host),
  // at the bounds now that the page's place and scale are known.
  SetWindowPos(v->host, HWND_TOP, 0, 0, 0, 0,
               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  ApplyBounds(v);
  hr = env10->CreateCoreWebView2ControllerWithOptions(
      v->host, options.Get(),
      Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
          [id](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
            OnControllerCreated(id, result, controller);
            return S_OK;
          })
          .Get());
  if (FAILED(hr))
    return hr;
  v->creating = true;
  return S_OK;
}

void Fail(uint32_t id, const std::string& message) {
  ChildView* v = Find(id);
  if (!v)
    return;
  SetLoad(v, "failed", v->pending_url, "failed", message);
  Close(id);
}

void Abandon(uint32_t id) {
  auto it = Views().find(id);
  if (it != Views().end()) {
    std::unique_ptr<ChildView> v = std::move(it->second);
    Views().erase(it);
    if (v->host)
      DestroyWindow(v->host);
  }
  WebViewSinks::Instance().Abandon(id);
}

bool SetBounds(uint32_t id, double x, double y, double width, double height) {
  ChildView* v = Find(id);
  if (!v)
    return false;
  v->bounds = {x, y, width, height};
  ApplyBounds(v);
  return true;
}

bool SetVisible(uint32_t id, bool visible) {
  ChildView* v = Find(id);
  if (!v)
    return false;
  if (!visible) {
    ReturnFocus(v, laufey_common::kWebViewFocusHidden);
    v = Find(id);
    if (!v)
      return false;
  }
  v->visible = visible;
  ApplyVisible(v);
  return true;
}

int32_t Navigate(uint32_t id, const std::string& url) {
  ChildView* v = Find(id);
  if (!v)
    return LAUFEY_WEBVIEW_CLOSED;
  if (!v->webview || !v->first_load) {
    v->pending_url = url;  // loaded once the controller is ready
    return LAUFEY_WEBVIEW_OK;
  }
  std::wstring wurl = Wide(url);
  if (FAILED(v->webview->Navigate(wurl.c_str())))
    return LAUFEY_WEBVIEW_NOT_ALLOWED_URL;
  return LAUFEY_WEBVIEW_OK;
}

bool Command(uint32_t id, int32_t command) {
  ChildView* v = Find(id);
  if (!v)
    return false;
  if (command == LAUFEY_WEBVIEW_COMMAND_BLUR) {
    ReturnFocus(v, laufey_common::kWebViewFocusBlur);
    return true;
  }
  if (!v->webview)
    return command != LAUFEY_WEBVIEW_COMMAND_FOCUS;
  ICoreWebView2* w = v->webview.Get();
  BOOL can = FALSE;
  switch (command) {
    case LAUFEY_WEBVIEW_COMMAND_BACK:
      if (SUCCEEDED(w->get_CanGoBack(&can)) && can)
        w->GoBack();
      return true;
    case LAUFEY_WEBVIEW_COMMAND_FORWARD:
      if (SUCCEEDED(w->get_CanGoForward(&can)) && can)
        w->GoForward();
      return true;
    case LAUFEY_WEBVIEW_COMMAND_RELOAD:
      w->Reload();
      return true;
    case LAUFEY_WEBVIEW_COMMAND_RELOAD_IGNORE_CACHE:
      // WebView2 has no reload that bypasses the cache; DevTools does.
      SendDevTools(id, "Page.reload", "{\"ignoreCache\":true}",
                   [](bool, laufey::ValuePtr) {});
      return true;
    case LAUFEY_WEBVIEW_COMMAND_STOP:
      w->Stop();
      return true;
    case LAUFEY_WEBVIEW_COMMAND_FOCUS:
      if (!v->visible || !v->controller)
        return false;
      return SUCCEEDED(v->controller->MoveFocus(
          COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC));
  }
  return false;
}

std::string State(uint32_t id) {
  ChildView* v = Find(id);
  if (!v)
    return "";
  laufey_common::WebViewStateFields s;
  s.url = CurrentUrl(v);
  if (s.url.empty() || (s.url == "about:blank" && !v->url_sent))
    s.url = v->pending_url;
  s.title = v->title;
  s.load = v->load;
  if (v->webview) {
    BOOL back = FALSE, forward = FALSE;
    v->webview->get_CanGoBack(&back);
    v->webview->get_CanGoForward(&forward);
    s.can_go_back = back != FALSE;
    s.can_go_forward = forward != FALSE;
  }
  s.visible = v->visible;
  s.bounds = v->bounds;
  s.z_order = laufey_common::WebViewSinks::Instance().ZOrderOf(id);
  return laufey_common::WebViewStateJson(s);
}

void Evaluate(uint32_t id, const std::string& script,
              laufey_webview_eval_fn callback, void* user_data) {
  if (!Find(id)) {
    callback(user_data, LAUFEY_WEBVIEW_CLOSED, "the web view is closed");
    return;
  }
  auto done = [callback, user_data](int32_t status, std::string text) {
    callback(user_data, status, text.c_str());
  };
  auto fail = [id, done](const laufey::ValuePtr& r, const char* what) {
    done(Find(id) ? LAUFEY_WEBVIEW_FAILED : LAUFEY_WEBVIEW_CLOSED,
         ErrorMessage(r, what));
  };
  SendDevTools(
      id, "Page.getFrameTree", "{}",
      [id, script, done, fail](bool ok, laufey::ValuePtr r) {
        laufey::ValuePtr frame = Field(Field(r, "frameTree"), "frame");
        if (!ok || !HasString(frame, "id")) {
          fail(r, "the page has no frame to run the script in");
          return;
        }
        std::string world = "{\"frameId\":" + Quote(StringField(frame, "id")) +
                            ",\"worldName\":\"laufey-webview\"}";
        SendDevTools(
            id, "Page.createIsolatedWorld", world,
            [id, script, done, fail](bool ok, laufey::ValuePtr r) {
              int context_id = 0;
              if (!ok || !IntField(r, "executionContextId", &context_id)) {
                fail(r, "the script's world could not be made");
                return;
              }
              std::string eval =
                  "{\"expression\":" + Quote(script) +
                  ",\"contextId\":" + std::to_string(context_id) +
                  ",\"awaitPromise\":true,\"returnByValue\":false,"
                  // DevTools' own default, stated: no page CSP refuses it.
                  "\"allowUnsafeEvalBlockedByCSP\":true,"
                  "\"objectGroup\":" +
                  Quote(kEvalGroup) + "}";
              SendDevTools(
                  id, "Runtime.evaluate", eval,
                  [id, context_id, done, fail](bool ok, laufey::ValuePtr r) {
                    if (!ok) {
                      fail(r, "the script could not be run");
                      return;
                    }
                    laufey::ValuePtr details = Field(r, "exceptionDetails");
                    if (details && details->IsDict()) {
                      done(LAUFEY_WEBVIEW_SCRIPT_ERROR, ExceptionText(details));
                      return;
                    }
                    laufey::ValuePtr result = Field(r, "result");
                    if (!result || !result->IsDict()) {
                      fail(r, "the script's result could not be read");
                      return;
                    }
                    EvalSerialize(id, context_id, result, done);
                  });
            });
      });
}

namespace {

double NumberField(const laufey::ValuePtr& dict, const char* key) {
  laufey::ValuePtr v = Field(dict, key);
  if (v && v->IsDouble() && std::isfinite(v->GetDouble()))
    return v->GetDouble();
  if (v && v->IsInt())
    return static_cast<double>(v->GetInt());
  return 0;
}

std::string JsonNumber(double d) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.6f", d);
  for (char* p = buf; *p; ++p) {
    if (*p == ',')
      *p = '.';
  }
  return buf;
}

// `params` is captureScreenshot's JSON object without its closing brace.
void CaptureWith(uint32_t id, std::string params,
                 laufey_webview_capture_fn callback, void* user_data) {
  params += "}";
  SendDevTools(
      id, "Page.captureScreenshot", params,
      [id, callback, user_data](bool ok, laufey::ValuePtr r) {
        if (!ok || !HasString(r, "data")) {
          std::string message =
              ErrorMessage(r, "the page could not be captured");
          callback(user_data,
                   Find(id) ? LAUFEY_WEBVIEW_FAILED : LAUFEY_WEBVIEW_CLOSED,
                   nullptr, 0, message.c_str());
          return;
        }
        std::vector<uint8_t> data;
        if (!laufey_common::Base64Decode(StringField(r, "data"), &data) ||
            data.empty()) {
          callback(user_data, LAUFEY_WEBVIEW_FAILED, nullptr, 0,
                   "the image could not be decoded");
          return;
        }
        callback(user_data, LAUFEY_WEBVIEW_OK, data.data(), data.size(),
                 nullptr);
      });
}

}  // namespace

void Capture(uint32_t id, int32_t format, int32_t quality, double scale,
             laufey_webview_capture_fn callback, void* user_data) {
  if (!Find(id)) {
    callback(user_data, LAUFEY_WEBVIEW_CLOSED, nullptr, 0,
             "the web view is closed");
    return;
  }
  bool jpeg = format == LAUFEY_WEBVIEW_CAPTURE_JPEG;
  std::string params = std::string("{\"format\":\"") + (jpeg ? "jpeg" : "png") +
                       "\"" +
                       (jpeg ? ",\"quality\":" + std::to_string(quality) : "") +
                       ",\"fromSurface\":true";
  if (!(scale > 0)) {
    CaptureWith(id, params, callback, user_data);
    return;
  }
  // A scale: clip to the visual viewport at scale / devicePixelRatio (the
  // screenshot's pixels are clip CSS px x clip.scale x the device scale).
  SendDevTools(
      id, "Page.getLayoutMetrics", "{}",
      [id, params, scale, callback, user_data](bool ok, laufey::ValuePtr r) {
        std::string p = params;
        laufey::ValuePtr css = ok ? Field(r, "cssVisualViewport") : nullptr;
        laufey::ValuePtr device = ok ? Field(r, "visualViewport") : nullptr;
        double w = NumberField(css, "clientWidth");
        double h = NumberField(css, "clientHeight");
        if (w > 0 && h > 0) {
          double dpr = NumberField(device, "clientWidth") / w;
          double clip_scale = dpr > 0 ? scale / dpr : scale;
          p += ",\"clip\":{\"x\":" + JsonNumber(NumberField(css, "pageX")) +
               ",\"y\":" + JsonNumber(NumberField(css, "pageY")) +
               ",\"width\":" + JsonNumber(w) + ",\"height\":" + JsonNumber(h) +
               ",\"scale\":" + JsonNumber(clip_scale) + "}";
        }
        CaptureWith(id, p, callback, user_data);
      });
}

bool Restack(const std::vector<uint32_t>& bottom_to_top) {
  bool all = true;
  for (uint32_t id : bottom_to_top) {
    ChildView* v = Find(id);
    if (!v || !v->host) {
      all = false;
      continue;
    }
    // Each in turn to the top of its siblings: the last one ends up highest.
    SetWindowPos(v->host, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  return all;
}

bool Close(uint32_t id) {
  auto it = Views().find(id);
  if (it == Views().end())
    return false;
  std::unique_ptr<ChildView> v = std::move(it->second);
  Views().erase(it);
  // Every pending evaluate / capture answers now (CLOSED: the view is gone).
  FailPending(v.get(), "the web view is closed");
  // Its downloads are cancelled and its permission requests refused, each
  // with its "cancelled" before CLOSED (late engine callbacks find no view).
  EndDownloadsAndPermissions(v.get());
  if (v->console_receiver)
    v->console_receiver->remove_DevToolsProtocolEventReceived(v->console_token);
  if (v->exception_receiver)
    v->exception_receiver->remove_DevToolsProtocolEventReceived(
        v->exception_token);
  ReturnFocus(v.get());
  if (v->controller)
    v->controller->Close();
  if (v->host)
    DestroyWindow(v->host);
  WebViewSinks::Instance().Close(id);
  return true;
}

bool SetForwardKeys(uint32_t id, const std::vector<std::string>& keys) {
  ChildView* v = Find(id);
  if (!v)
    return false;
  v->options.forward_keys = keys;
  return true;
}

int32_t DownloadControl(uint32_t id, uint32_t download_id, int32_t action,
                        const std::string& path, const SaveDialogShow& show,
                        const UiPost& post) {
  ChildView* v = Find(id);
  if (!v)
    return LAUFEY_WEBVIEW_CLOSED;
  ChildDownload* d = FindDownload(v, download_id);
  if (!d)
    return LAUFEY_WEBVIEW_INVALID;
  int32_t accepted =
      laufey_common::WebViewDownloadAccepts(d->phase, action, true);
  if (accepted != LAUFEY_WEBVIEW_OK)
    return accepted;
  switch (action) {
    case LAUFEY_WEBVIEW_DOWNLOAD_SAVE:
      return ApplySave(v, d, path);
    case LAUFEY_WEBVIEW_DOWNLOAD_SAVE_AS: {
      d->phase = WebViewDownloadPhase::kChoosing;
      uint32_t window_id = v->window_id;
      std::string name = d->filename;
      laufey_common::DownloadDialogShow dialog;
      if (show) {
        dialog = [show, window_id](const laufey_file_dialog_options_t* options,
                                   laufey_file_dialog_result_fn callback,
                                   void* user_data) {
          return show(window_id, options, callback, user_data);
        };
      }
      // The dialog answers on laufey's dialog thread; the view's objects
      // belong to the UI thread. `v` and `d` are not used past this call.
      UiPost to_ui = post;
      laufey_common::AskDownloadDestination(
          name, dialog, [id, download_id, to_ui](std::string chosen) {
            std::function<void()> task = [id, download_id, chosen] {
              OnSaveAsChosen(id, download_id, chosen);
            };
            if (to_ui)
              to_ui(std::move(task));
            else
              task();
          });
      return LAUFEY_WEBVIEW_OK;
    }
    case LAUFEY_WEBVIEW_DOWNLOAD_CANCEL:
      CancelDownload(v, download_id);
      return LAUFEY_WEBVIEW_OK;
    case LAUFEY_WEBVIEW_DOWNLOAD_PAUSE: {
      if (!d->operation || FAILED(d->operation->Pause()))
        return LAUFEY_WEBVIEW_FAILED;
      d->phase = WebViewDownloadPhase::kPaused;
      d->pause_in_flight = true;
      d->resume_pending = false;
      d->resume_in_flight = false;
      laufey_common::WebViewDownloadFields f =
          DownloadFields(*d, laufey_common::kWebViewDownloadProgressing);
      f.paused = true;
      f.can_resume = CanResume(*d);
      EmitDownload(id, f);
      return LAUFEY_WEBVIEW_OK;
    }
    case LAUFEY_WEBVIEW_DOWNLOAD_RESUME: {
      if (!d->operation)
        return LAUFEY_WEBVIEW_FAILED;
      COREWEBVIEW2_DOWNLOAD_STATE state =
          COREWEBVIEW2_DOWNLOAD_STATE_IN_PROGRESS;
      if (d->pause_in_flight && SUCCEEDED(d->operation->get_State(&state)) &&
          state == COREWEBVIEW2_DOWNLOAD_STATE_INTERRUPTED)
        d->pause_in_flight = false;  // it took effect; its event is queued
      if (d->pause_in_flight) {
        // The pause hasn't taken effect yet: resumed once it has.
        d->resume_pending = true;
      } else if (FAILED(d->operation->Resume())) {
        return LAUFEY_WEBVIEW_FAILED;
      } else {
        d->resume_in_flight = true;
      }
      d->phase = WebViewDownloadPhase::kProgressing;
      ArmDownloadPoll(d, NowMs());
      laufey_common::WebViewDownloadFields f =
          DownloadFields(*d, laufey_common::kWebViewDownloadProgressing);
      f.can_resume = CanResume(*d);
      EmitDownload(id, f);
      return LAUFEY_WEBVIEW_OK;
    }
  }
  return LAUFEY_WEBVIEW_INVALID;
}

int32_t PermissionRespond(uint32_t id, uint32_t request_id, bool allow) {
  ChildView* v = Find(id);
  if (!v)
    return LAUFEY_WEBVIEW_CLOSED;
  auto it = v->permissions.find(request_id);
  if (it == v->permissions.end())
    return LAUFEY_WEBVIEW_INVALID;
  ChildPermission p = std::move(it->second);
  v->permissions.erase(it);
  ResolvePermission(&p, allow);
  return LAUFEY_WEBVIEW_OK;
}

void RelayoutForWindow(uint32_t window_id) {
  for (auto& kv : Views()) {
    if (kv.second->window_id == window_id)
      ApplyBounds(kv.second.get());
  }
}

void CloseAllForWindow(uint32_t window_id) {
  std::vector<uint32_t> ids;
  for (const auto& kv : Views()) {
    if (kv.second->window_id == window_id)
      ids.push_back(kv.first);
  }
  for (uint32_t id : ids)
    Close(id);
}

}  // namespace laufey_child_webview_win
