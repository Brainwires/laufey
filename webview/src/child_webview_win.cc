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
#include "laufey_json.h"
#include "laufey_launch_config.h"
#include "laufey_navigation_guard.h"
#include "laufey_notifications.h"

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
  std::string sent_url;
  bool sent_back = false;
  bool sent_forward = false;
  bool url_sent = false;
  int64_t console_second = 0;
  int console_count = 0;
  int next_call = 1;
  std::map<int, DevToolsCallback> pending;
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

void NoteFocus(uint32_t id, bool focused) {
  ChildView* v = Find(id);
  if (!v || v->focused == focused)
    return;
  v->focused = focused;
  Emit(id, LAUFEY_WEBVIEW_EVENT_FOCUS,
       laufey_common::WebViewFocusJson(focused));
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

// Gives the keyboard focus back to the window's page (the view is hidden or
// closing while it has it).
void ReturnFocus(ChildView* v) {
  if (v->focused && v->page)
    v->page->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
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
              }
              args->put_Cancel(TRUE);
              return S_OK;
            })
            .Get(),
        nullptr);
  }

  // "ask": WebView2's own prompt (the default); "deny": refused without
  // one, and the refusal is not saved in the partition's profile.
  webview->add_PermissionRequested(
      Callback<ICoreWebView2PermissionRequestedEventHandler>(
          [id](ICoreWebView2*,
               ICoreWebView2PermissionRequestedEventArgs* args) -> HRESULT {
            ChildView* v = Find(id);
            if (v && v->options.permissions ==
                         laufey_common::WebViewPermissions::kAsk)
              return S_OK;
            ComPtr<ICoreWebView2PermissionRequestedEventArgs3> args3;
            if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args3))) && args3)
              args3->put_SavesInProfile(FALSE);
            args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
            return S_OK;
          })
          .Get(),
      nullptr);

  // Screen capture raises no PermissionRequested (WebView2 shows its source
  // picker): under "deny" it is refused before the picker.
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
         LAUFEY_WEBVIEW_CAP_PERSISTENT_PARTITIONS |
         LAUFEY_WEBVIEW_CAP_PERMISSION_PROMPTS |
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
  if (!visible)
    ReturnFocus(v);
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
