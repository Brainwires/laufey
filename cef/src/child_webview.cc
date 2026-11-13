// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Child web views on CEF (API 58); see child_webview.h and docs/webviews.md.
//
// Isolation: each view's browser has its own CefClient (ChildClient), never
// LaufeyHandler, so none of the window's machinery reaches it: no JS-call
// dispatch (every process message from its renderer is swallowed), no
// navigation guard or will-navigate, no scheme handlers (those are
// registered on the global request context; a view's partition is a
// CefRequestContext of its own), and the renderer gives it no bridge
// (extra_info's kExtraInfoKey; render_process_handler.cc). Its main frame may
// only load http(s) (and about:blank).
//
// Evaluate and capture go through the browser's DevTools protocol
// (ExecuteDevToolsMethod): Runtime.evaluate in an isolated world made with
// Page.createIsolatedWorld (no page CSP applies), and Page.captureScreenshot.

#include "child_webview.h"

#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "app.h"
#include "include/base/cef_callback.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_parser.h"
#include "include/cef_request_context.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_overlay_controller.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"
#include "laufey_launch_config.h"
#include "laufey_navigation_guard.h"

namespace laufey_cef_child_webview {

namespace {

using laufey_common::WebViewOptions;
using laufey_common::WebViewSinks;

// A DevTools method's answer: `success` and the result (or error) object.
using DevToolsCallback =
    std::function<void(bool success, CefRefPtr<CefDictionaryValue> result)>;

constexpr int kDevToolsTimeoutMs = 15000;
constexpr int kConsolePerSecond = 200;
constexpr size_t kConsoleMaxBytes = 16 * 1024;

class ChildClient;

struct ChildView {
  uint32_t id = 0;
  uint32_t window_id = 0;
  CefRefPtr<CefBrowserView> view;
  CefRefPtr<CefBrowserView> page_view;
  CefRefPtr<CefOverlayController> overlay;
  CefRefPtr<CefBrowser> browser;  // from OnAfterCreated
  CefRefPtr<ChildClient> client;
  CefRefPtr<CefRegistration> devtools;
  WebViewOptions options;
  std::string pending_url;  // loaded once the browser exists
  const char* load = "complete";
  bool suppress_load_events = false;  // after "failed", until a navigation
  std::string title;
  laufey_common::WebViewBounds bounds;
  bool visible = true;
  bool focused = false;
  bool closing = false;
  std::string sent_url;
  bool sent_back = false;
  bool sent_forward = false;
  bool url_sent = false;
  int64_t console_second = 0;
  int console_count = 0;
  std::map<int, DevToolsCallback> pending;
};

// UI thread only.
std::map<uint32_t, std::unique_ptr<ChildView>>& Views() {
  static auto* views = new std::map<uint32_t, std::unique_ptr<ChildView>>();
  return *views;
}

// The view, or nullptr when unknown or closing.
ChildView* Find(uint32_t id) {
  auto it = Views().find(id);
  if (it == Views().end() || it->second->closing)
    return nullptr;
  return it->second.get();
}

// Partitions: one request context per name (UI thread only).
std::map<std::string, CefRefPtr<CefRequestContext>>& Contexts() {
  static auto* contexts =
      new std::map<std::string, CefRefPtr<CefRequestContext>>();
  return *contexts;
}

std::string RootCachePath() {
  CefRefPtr<CefRequestContext> global = CefRequestContext::GetGlobalContext();
  return global ? global->GetCachePath().ToString() : std::string();
}

CefRefPtr<CefRequestContext> ContextFor(const std::string& partition) {
  auto it = Contexts().find(partition);
  if (it != Contexts().end())
    return it->second;
  CefRequestContextSettings settings;
  std::string root = RootCachePath();
  if (!laufey_common::IsEphemeralPartition(partition) && !root.empty()) {
#if defined(_WIN32)
    const char* sep = "\\";
#else
    const char* sep = "/";
#endif
    std::string path = root + sep + "WebViews" + sep +
                       laufey_common::PartitionDirName(partition);
    CefString(&settings.cache_path) = path;
    settings.persist_session_cookies = true;
  }
  // An empty cache_path is an in-memory ("incognito") context.
  CefRefPtr<CefRequestContext> context =
      CefRequestContext::CreateContext(settings, nullptr);
  Contexts()[partition] = context;
  return context;
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

std::string CurrentUrl(ChildView* v) {
  if (!v->browser)
    return v->pending_url;
  CefRefPtr<CefFrame> main = v->browser->GetMainFrame();
  return main ? main->GetURL().ToString() : std::string();
}

void MaybeSendUrl(ChildView* v) {
  if (!v->browser)
    return;
  std::string url = CurrentUrl(v);
  if (url.empty() || url == "about:blank")
    return;
  bool back = v->browser->CanGoBack();
  bool forward = v->browser->CanGoForward();
  if (v->url_sent && url == v->sent_url && back == v->sent_back &&
      forward == v->sent_forward)
    return;
  v->url_sent = true;
  v->sent_url = url;
  v->sent_back = back;
  v->sent_forward = forward;
  Emit(v->id, LAUFEY_WEBVIEW_EVENT_URL,
       laufey_common::WebViewUrlJson(url, back, forward));
}

void OpenInBrowser(const std::string& url) {
  if (laufey_common::IsAllowedWebViewUrl(url))
    LaufeyOpenExternalURL(url);
}

// A refused popup / new-tab request under the view's popup policy.
void HandlePopup(ChildView* v, const std::string& url, bool popup) {
  switch (v->options.popups) {
    case laufey_common::WebViewPopups::kSameView:
      if (v->browser && laufey_common::IsAllowedWebViewUrl(url))
        v->browser->GetMainFrame()->LoadURL(url);
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

// net::Error to the engine-neutral codes; nullptr: not a failure (aborted).
const char* LoadErrorCode(int code) {
  if (code == ERR_ABORTED)
    return nullptr;
  if (code == ERR_NAME_NOT_RESOLVED || code == ERR_NAME_RESOLUTION_FAILED)
    return "dns";
  if (code <= -200 && code > -300)  // certificate errors
    return "tls";
  if (code == ERR_SSL_PROTOCOL_ERROR ||
      code == ERR_SSL_VERSION_OR_CIPHER_MISMATCH)
    return "tls";
  if (code == ERR_BLOCKED_BY_CLIENT || code == ERR_BLOCKED_BY_ADMINISTRATOR ||
      code == ERR_BLOCKED_BY_RESPONSE || code == ERR_UNSAFE_REDIRECT ||
      code == ERR_UNSAFE_PORT || code == ERR_DISALLOWED_URL_SCHEME)
    return "blocked";
  if (code <= -100 && code > -200)  // connection errors
    return "network";
  if (code == ERR_TIMED_OUT || code == ERR_INTERNET_DISCONNECTED ||
      code == ERR_NETWORK_CHANGED || code == ERR_ADDRESS_UNREACHABLE)
    return "network";
  return "failed";
}

void NoteFocus(uint32_t id, bool focused) {
  ChildView* v = Find(id);
  if (!v || v->focused == focused)
    return;
  v->focused = focused;
  Emit(id, LAUFEY_WEBVIEW_EVENT_FOCUS,
       laufey_common::WebViewFocusJson(focused));
}

void FailPending(ChildView* v, const char* message) {
  auto pending = std::move(v->pending);
  v->pending.clear();
  for (auto& kv : pending) {
    CefRefPtr<CefDictionaryValue> error = CefDictionaryValue::Create();
    error->SetString("message", message);
    kv.second(false, error);
  }
}

// Sends a DevTools method; `callback` runs exactly once (UI thread): with
// the result, the protocol's error, or a timeout / closed error.
void SendDevTools(uint32_t id, const std::string& method,
                  CefRefPtr<CefDictionaryValue> params,
                  DevToolsCallback callback) {
  ChildView* v = Find(id);
  if (!v || !v->browser) {
    CefRefPtr<CefDictionaryValue> error = CefDictionaryValue::Create();
    error->SetString("message", v ? "the page is not ready" : "closed");
    callback(false, error);
    return;
  }
  int message_id =
      v->browser->GetHost()->ExecuteDevToolsMethod(0, method, params);
  if (message_id <= 0) {
    CefRefPtr<CefDictionaryValue> error = CefDictionaryValue::Create();
    error->SetString("message", "the DevTools method could not be sent");
    callback(false, error);
    return;
  }
  v->pending[message_id] = std::move(callback);
  CefPostDelayedTask(
      TID_UI,
      base::BindOnce(
          [](uint32_t view_id, int mid) {
            auto it = Views().find(view_id);
            if (it == Views().end())
              return;
            auto p = it->second->pending.find(mid);
            if (p == it->second->pending.end())
              return;
            DevToolsCallback cb = std::move(p->second);
            it->second->pending.erase(p);
            CefRefPtr<CefDictionaryValue> error = CefDictionaryValue::Create();
            error->SetString("message", "the page did not answer in time");
            cb(false, error);
          },
          id, message_id),
      kDevToolsTimeoutMs);
}

std::string ErrorMessage(CefRefPtr<CefDictionaryValue> d,
                         const char* fallback) {
  if (d && d->GetType("message") == VTYPE_STRING)
    return d->GetString("message").ToString();
  return fallback;
}

// The text of a Runtime exceptionDetails object.
std::string ExceptionText(CefRefPtr<CefDictionaryValue> details) {
  if (!details)
    return "the script threw";
  if (details->GetType("exception") == VTYPE_DICTIONARY) {
    CefRefPtr<CefDictionaryValue> e = details->GetDictionary("exception");
    if (e->GetType("description") == VTYPE_STRING) {
      std::string d = e->GetString("description").ToString();
      // The first line ("Error: x"), not the stack.
      size_t nl = d.find('\n');
      return nl == std::string::npos ? d : d.substr(0, nl);
    }
    if (e->GetType("value") == VTYPE_STRING)
      return e->GetString("value").ToString();
  }
  if (details->GetType("text") == VTYPE_STRING)
    return details->GetString("text").ToString();
  return "the script threw";
}

const char* ConsoleLevel(cef_log_severity_t level) {
  switch (level) {
    case LOGSEVERITY_VERBOSE:
      return "debug";
    case LOGSEVERITY_WARNING:
      return "warn";
    case LOGSEVERITY_ERROR:
    case LOGSEVERITY_FATAL:
      return "error";
    default:
      return "log";
  }
}

std::string PermissionText(uint32_t media, uint32_t prompt) {
  std::vector<std::string> what;
  if ((media & (CEF_MEDIA_PERMISSION_DEVICE_VIDEO_CAPTURE |
                CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE)) ||
      (prompt & CEF_PERMISSION_TYPE_CAMERA_STREAM))
    what.push_back("use your camera");
  if ((media & (CEF_MEDIA_PERMISSION_DEVICE_AUDIO_CAPTURE |
                CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE)) ||
      (prompt & CEF_PERMISSION_TYPE_MIC_STREAM))
    what.push_back("use your microphone");
  if (media & (CEF_MEDIA_PERMISSION_DESKTOP_VIDEO_CAPTURE |
               CEF_MEDIA_PERMISSION_DESKTOP_AUDIO_CAPTURE))
    what.push_back("record your screen");
  if (prompt & CEF_PERMISSION_TYPE_GEOLOCATION)
    what.push_back("know your location");
  if (prompt & CEF_PERMISSION_TYPE_NOTIFICATIONS)
    what.push_back("show notifications");
  if (what.empty())
    what.push_back("use a device or a browser feature");
  std::string out;
  for (size_t i = 0; i < what.size(); ++i) {
    if (i > 0)
      out += i + 1 == what.size() ? " and " : ", ";
    out += what[i];
  }
  return out;
}

// Asks the user with a message dialog; `done(allowed)` runs exactly once.
void AskPermission(uint32_t window_id, const std::string& origin,
                   const std::string& what, std::function<void(bool)> done) {
  struct Ctx {
    std::function<void(bool)> done;
  };
  auto* ctx = new Ctx{std::move(done)};
  std::string message = origin + " wants to " + what + ".";
  const char* buttons[] = {"Allow", "Deny"};
  laufey_message_dialog_options_t options{};
  options.kind = LAUFEY_MESSAGE_DIALOG_WARNING;
  options.message = message.c_str();
  options.buttons = buttons;
  options.button_count = 2;
  options.default_index = 1;
  options.cancel_index = 1;
  uint32_t dialog = LaufeyShowMessageDialogForWindow(
      window_id, &options,
      [](void* user_data, uint32_t, int status, int32_t response) {
        auto* c = static_cast<Ctx*>(user_data);
        bool allowed =
            status == LAUFEY_MESSAGE_DIALOG_ANSWERED && response == 0;
        // The dialog may answer on another thread: decide on the UI thread.
        CefPostTask(TID_UI, base::BindOnce(
                                [](Ctx* c, bool allowed) {
                                  c->done(allowed);
                                  delete c;
                                },
                                c, allowed));
      },
      ctx);
  if (!dialog) {
    ctx->done(false);
    delete ctx;
  }
}

// Every browser of a child view shares this client type (one per view).
class ChildClient : public CefClient,
                    public CefLifeSpanHandler,
                    public CefDisplayHandler,
                    public CefLoadHandler,
                    public CefRequestHandler,
                    public CefDownloadHandler,
                    public CefPermissionHandler,
                    public CefContextMenuHandler,
                    public CefDevToolsMessageObserver {
 public:
  explicit ChildClient(uint32_t id) : id_(id) {}

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override {
    return this;
  }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override {
    return this;
  }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override {
    return this;
  }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override {
    return this;
  }
  CefRefPtr<CefDownloadHandler> GetDownloadHandler() override {
    return this;
  }
  CefRefPtr<CefPermissionHandler> GetPermissionHandler() override {
    return this;
  }
  CefRefPtr<CefContextMenuHandler> GetContextMenuHandler() override {
    return this;
  }

  // Nothing a child page's renderer sends reaches laufey: it has no bridge,
  // and a compromised renderer's "laufey_call" is dropped here.
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                                CefProcessId,
                                CefRefPtr<CefProcessMessage>) override {
    return true;
  }

  // --- CefLifeSpanHandler ---

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override {
    auto it = Views().find(id_);
    if (it == Views().end()) {
      browser->GetHost()->CloseBrowser(true);
      return;
    }
    ChildView* v = it->second.get();
    v->browser = browser;
    if (v->closing) {
      browser->GetHost()->CloseBrowser(true);
      return;
    }
    v->devtools = browser->GetHost()->AddDevToolsMessageObserver(this);
    uint32_t id = id_;
    auto load_first = [id](bool, CefRefPtr<CefDictionaryValue>) {
      ChildView* view = Find(id);
      if (view && view->browser && !view->pending_url.empty()) {
        std::string url = view->pending_url;
        view->browser->GetMainFrame()->LoadURL(url);
      }
    };
    if (!v->options.user_agent.empty()) {
      // Before the first load: navigator.userAgent and the request header.
      CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
      params->SetString("userAgent", v->options.user_agent);
      SendDevTools(id, "Emulation.setUserAgentOverride", params, load_first);
    } else {
      load_first(true, nullptr);
    }
  }

  bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame>, int,
                     const CefString& target_url, const CefString&,
                     WindowOpenDisposition target_disposition, bool,
                     const CefPopupFeatures&, CefWindowInfo&,
                     CefRefPtr<CefClient>&, CefBrowserSettings&,
                     CefRefPtr<CefDictionaryValue>&, bool*) override {
    // No second browser ever: the policy decides where the URL goes.
    if (ChildView* v = Find(id_))
      HandlePopup(v, target_url.ToString(),
                  target_disposition == CEF_WOD_NEW_POPUP);
    return true;
  }

  bool DoClose(CefRefPtr<CefBrowser>) override {
    return false;
  }

  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override {
    auto it = Views().find(id_);
    if (it == Views().end())
      return;
    ChildView* v = it->second.get();
    FailPending(v, "the web view is closed");
    v->devtools = nullptr;
    if (v->overlay && v->overlay->IsValid())
      v->overlay->Destroy();
    // Closed by the browser itself (not through Close): CLOSED now.
    WebViewSinks::Instance().Close(id_);
    Views().erase(it);
  }

  // --- CefDisplayHandler ---

  void OnTitleChange(CefRefPtr<CefBrowser>, const CefString& title) override {
    ChildView* v = Find(id_);
    if (!v)
      return;
    v->title = title.ToString();
    Emit(id_, LAUFEY_WEBVIEW_EVENT_TITLE,
         laufey_common::WebViewTitleJson(v->title));
  }

  void OnAddressChange(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                       const CefString&) override {
    ChildView* v = Find(id_);
    if (v && frame && frame->IsMain())
      MaybeSendUrl(v);
  }

  bool OnConsoleMessage(CefRefPtr<CefBrowser>, cef_log_severity_t level,
                        const CefString& message, const CefString& source,
                        int line) override {
    ChildView* v = Find(id_);
    if (!v)
      return false;
    int64_t second = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    if (second != v->console_second) {
      v->console_second = second;
      v->console_count = 0;
    }
    if (++v->console_count > kConsolePerSecond)
      return false;
    std::string text = message.ToString();
    if (text.size() > kConsoleMaxBytes)
      text.resize(kConsoleMaxBytes);
    Emit(id_, LAUFEY_WEBVIEW_EVENT_CONSOLE,
         laufey_common::WebViewConsoleJson(ConsoleLevel(level), text,
                                           source.ToString(), line));
    return false;
  }

  // --- CefLoadHandler ---

  void OnLoadingStateChange(CefRefPtr<CefBrowser>, bool isLoading, bool,
                            bool) override {
    ChildView* v = Find(id_);
    if (!v)
      return;
    MaybeSendUrl(v);
    if (!isLoading && strcmp(v->load, "loading") == 0)
      v->load = "complete";  // stopped
  }

  void OnLoadStart(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                   TransitionType) override {
    ChildView* v = Find(id_);
    if (!v || !frame || !frame->IsMain() || v->suppress_load_events)
      return;
    std::string url = frame->GetURL().ToString();
    if (url == "about:blank" && !v->url_sent)
      return;  // the blank page every browser starts on
    SetLoad(v, "interactive", url);
  }

  void OnLoadEnd(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                 int) override {
    ChildView* v = Find(id_);
    if (!v || !frame || !frame->IsMain() || v->suppress_load_events)
      return;
    std::string url = frame->GetURL().ToString();
    if (url == "about:blank" && !v->url_sent)
      return;
    SetLoad(v, "complete", url);
  }

  void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                   ErrorCode errorCode, const CefString& errorText,
                   const CefString& failedUrl) override {
    ChildView* v = Find(id_);
    if (!v || !frame || !frame->IsMain())
      return;
    const char* code = LoadErrorCode(errorCode);
    if (!code) {
      if (!browser->IsLoading())
        v->load = "complete";
      return;
    }
    std::string message = errorText.ToString();
    if (message.empty())
      message = "the page could not be loaded";
    SetLoad(v, "failed", failedUrl.ToString(), code, message);
    // The error page that may follow is not the page's load.
    v->suppress_load_events = true;
  }

  // --- CefRequestHandler ---

  bool OnBeforeBrowse(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request, bool,
                      bool is_redirect) override {
    ChildView* v = Find(id_);
    if (!v)
      return true;
    if (!frame || !frame->IsMain())
      return false;  // frames inside the page load what the page asks
    std::string url = request->GetURL().ToString();
    if (!laufey_common::IsAllowedWebViewNavigation(url))
      return true;  // cancel: file:, data:, javascript:, app schemes...
    if (!is_redirect && url != "about:blank") {
      v->suppress_load_events = false;
      SetLoad(v, "loading", url);
    }
    return false;
  }

  bool OnOpenURLFromTab(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                        const CefString& target_url,
                        WindowOpenDisposition target_disposition,
                        bool) override {
    if (ChildView* v = Find(id_))
      HandlePopup(v, target_url.ToString(),
                  target_disposition == CEF_WOD_NEW_POPUP);
    return true;
  }

  // --- CefDownloadHandler ---

  bool CanDownload(CefRefPtr<CefBrowser>, const CefString& url,
                   const CefString&) override {
    ChildView* v = Find(id_);
    if (!v)
      return false;
    switch (v->options.downloads) {
      case laufey_common::WebViewDownloads::kAsk:
        return true;
      case laufey_common::WebViewDownloads::kBrowser:
        OpenInBrowser(url.ToString());
        return false;
      case laufey_common::WebViewDownloads::kDeny:
        return false;
    }
    return false;
  }

  bool OnBeforeDownload(
      CefRefPtr<CefBrowser>, CefRefPtr<CefDownloadItem>, const CefString&,
      CefRefPtr<CefBeforeDownloadCallback> callback) override {
    ChildView* v = Find(id_);
    if (!v || v->options.downloads != laufey_common::WebViewDownloads::kAsk)
      return false;
    callback->Continue("", true);  // the OS save dialog
    return true;
  }

  // --- CefPermissionHandler ---

  bool OnRequestMediaAccessPermission(
      CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
      const CefString& requesting_origin, uint32_t requested_permissions,
      CefRefPtr<CefMediaAccessCallback> callback) override {
    ChildView* v = Find(id_);
    if (!v ||
        v->options.permissions != laufey_common::WebViewPermissions::kAsk) {
      callback->Cancel();
      return true;
    }
    AskPermission(v->window_id, requesting_origin.ToString(),
                  PermissionText(requested_permissions, 0),
                  [callback, requested_permissions](bool allowed) {
                    if (allowed)
                      callback->Continue(requested_permissions);
                    else
                      callback->Cancel();
                  });
    return true;
  }

  bool OnShowPermissionPrompt(
      CefRefPtr<CefBrowser>, uint64_t, const CefString& requesting_origin,
      uint32_t requested_permissions,
      CefRefPtr<CefPermissionPromptCallback> callback) override {
    ChildView* v = Find(id_);
    if (!v ||
        v->options.permissions != laufey_common::WebViewPermissions::kAsk) {
      callback->Continue(CEF_PERMISSION_RESULT_DENY);
      return true;
    }
    AskPermission(v->window_id, requesting_origin.ToString(),
                  PermissionText(0, requested_permissions),
                  [callback](bool allowed) {
                    callback->Continue(allowed ? CEF_PERMISSION_RESULT_ACCEPT
                                               : CEF_PERMISSION_RESULT_DENY);
                  });
    return true;
  }

  // --- CefContextMenuHandler ---

  void OnBeforeContextMenu(CefRefPtr<CefBrowser>, CefRefPtr<CefFrame>,
                           CefRefPtr<CefContextMenuParams>,
                           CefRefPtr<CefMenuModel> model) override {
    // view-source: is not an http(s) page this view may load.
    model->Remove(MENU_ID_VIEW_SOURCE);
  }

  // --- CefDevToolsMessageObserver ---

  void OnDevToolsMethodResult(CefRefPtr<CefBrowser>, int message_id,
                              bool success, const void* result,
                              size_t result_size) override {
    auto it = Views().find(id_);
    if (it == Views().end())
      return;
    auto p = it->second->pending.find(message_id);
    if (p == it->second->pending.end())
      return;
    DevToolsCallback cb = std::move(p->second);
    it->second->pending.erase(p);
    CefRefPtr<CefDictionaryValue> dict;
    CefRefPtr<CefValue> value =
        CefParseJSON(result, result_size, JSON_PARSER_RFC);
    if (value && value->GetType() == VTYPE_DICTIONARY)
      dict = value->GetDictionary();
    cb(success, dict ? dict : CefDictionaryValue::Create());
  }

 private:
  uint32_t id_;
  IMPLEMENT_REFCOUNTING(ChildClient);
};

class ChildViewDelegate : public CefBrowserViewDelegate {
 public:
  explicit ChildViewDelegate(uint32_t id) : id_(id) {}

  // Overlay browser views must be Alloy style (a Chrome-style browser needs
  // a window of its own).
  cef_runtime_style_t GetBrowserRuntimeStyle() override {
    return CEF_RUNTIME_STYLE_ALLOY;
  }
  void OnFocus(CefRefPtr<CefView>) override {
    NoteFocus(id_, true);
  }
  void OnBlur(CefRefPtr<CefView>) override {
    NoteFocus(id_, false);
  }

 private:
  uint32_t id_;
  IMPLEMENT_REFCOUNTING(ChildViewDelegate);
};

CefRect OverlayRect(ChildView* v) {
  CefRect page = v->page_view ? v->page_view->GetBounds() : CefRect();
  auto round = [](double d) {
    return static_cast<int>(d < 0 ? d - 0.5 : d + 0.5);
  };
  return CefRect(page.x + round(v->bounds.x), page.y + round(v->bounds.y),
                 round(v->bounds.width), round(v->bounds.height));
}

// Evaluate's steps: the main frame id, an isolated world in it, the script,
// then the result serialized in that world.
void EvalSerialize(uint32_t id, int context_id,
                   CefRefPtr<CefDictionaryValue> remote,
                   std::function<void(int32_t, std::string)> done) {
  CefRefPtr<CefDictionaryValue> arg = CefDictionaryValue::Create();
  if (remote->GetType("objectId") == VTYPE_STRING) {
    arg->SetString("objectId", remote->GetString("objectId"));
  } else if (remote->GetType("unserializableValue") == VTYPE_STRING) {
    arg->SetString("unserializableValue",
                   remote->GetString("unserializableValue"));
  } else if (remote->HasKey("value")) {
    arg->SetValue("value", remote->GetValue("value"));
  }  // else undefined
  CefRefPtr<CefListValue> args = CefListValue::Create();
  args->SetDictionary(0, arg);
  CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
  params->SetString("functionDeclaration",
                    laufey_common::kWebViewSerializeFunction);
  params->SetInt("executionContextId", context_id);
  params->SetList("arguments", args);
  params->SetBool("returnByValue", true);
  params->SetString("objectGroup", "laufey-webview-eval");
  SendDevTools(
      id, "Runtime.callFunctionOn", params,
      [id, done](bool success, CefRefPtr<CefDictionaryValue> r) {
        CefRefPtr<CefDictionaryValue> release = CefDictionaryValue::Create();
        release->SetString("objectGroup", "laufey-webview-eval");
        SendDevTools(id, "Runtime.releaseObjectGroup", release,
                     [](bool, CefRefPtr<CefDictionaryValue>) {});
        if (!success) {
          done(Find(id) ? LAUFEY_WEBVIEW_FAILED : LAUFEY_WEBVIEW_CLOSED,
               ErrorMessage(r, "the result could not be read"));
          return;
        }
        if (r->GetType("exceptionDetails") == VTYPE_DICTIONARY) {
          std::string text =
              ExceptionText(r->GetDictionary("exceptionDetails"));
          const std::string prefix = "TypeError: not_serializable: ";
          if (text.rfind(prefix, 0) == 0)
            text = text.substr(prefix.size());
          done(LAUFEY_WEBVIEW_NOT_SERIALIZABLE, text);
          return;
        }
        CefRefPtr<CefDictionaryValue> result =
            r->GetType("result") == VTYPE_DICTIONARY
                ? r->GetDictionary("result")
                : nullptr;
        if (!result || result->GetType("value") != VTYPE_STRING) {
          done(LAUFEY_WEBVIEW_FAILED, "the result could not be read");
          return;
        }
        done(LAUFEY_WEBVIEW_OK, result->GetString("value").ToString());
      });
}

}  // namespace

uint32_t Capabilities() {
  return LAUFEY_WEBVIEW_CAP_SUPPORTED | LAUFEY_WEBVIEW_CAP_CAPTURE |
         LAUFEY_WEBVIEW_CAP_EVALUATE | LAUFEY_WEBVIEW_CAP_PERMISSION_PROMPTS |
         (RootCachePath().empty() ? 0u
                                  : LAUFEY_WEBVIEW_CAP_PERSISTENT_PARTITIONS) |
#if defined(__APPLE__)
         // Overlays are child NSWindows there (Restack); Aura's on Windows
         // and Linux can't be reordered through CEF.
         LAUFEY_WEBVIEW_CAP_Z_ORDER |
#endif
         (LAUFEY_WEBVIEW_ENGINE_CEF << LAUFEY_WEBVIEW_ENGINE_SHIFT);
}

bool Restack(uint32_t window_id, const std::vector<uint32_t>& bottom_to_top) {
  CEF_REQUIRE_UI_THREAD();
#if defined(__APPLE__)
  std::vector<void*> views;
  for (uint32_t id : bottom_to_top) {
    ChildView* v = Find(id);
    if (v && v->browser)
      views.push_back(
          reinterpret_cast<void*>(v->browser->GetHost()->GetWindowHandle()));
  }
  return LaufeyCefRestackOverlaysMac(window_id, views);
#else
  (void)window_id;
  (void)bottom_to_top;
  return false;
#endif
}

uint32_t Create(uint32_t window_id, CefRefPtr<CefWindow> window,
                CefRefPtr<CefBrowserView> page_view,
                const WebViewOptions& options, laufey_webview_event_fn on_event,
                void* user_data, int32_t* status_out) {
  CEF_REQUIRE_UI_THREAD();
  if (!window || !page_view) {
    *status_out = LAUFEY_WEBVIEW_NO_WINDOW;
    return 0;
  }
  uint32_t id = WebViewSinks::Instance().Add(window_id, on_event, user_data);
  if (!id) {
    *status_out = LAUFEY_WEBVIEW_LIMIT;
    return 0;
  }
  auto v = std::make_unique<ChildView>();
  v->id = id;
  v->window_id = window_id;
  v->options = options;
  v->bounds = options.bounds;
  v->visible = options.visible;
  v->pending_url = options.url;
  v->page_view = page_view;
  v->client = new ChildClient(id);
  ChildView* raw = v.get();
  Views()[id] = std::move(v);

  CefBrowserSettings settings;
  CefRefPtr<CefDictionaryValue> extra_info = CefDictionaryValue::Create();
  extra_info->SetBool(kExtraInfoKey, true);
  // Starts blank; OnAfterCreated loads the URL (after the user agent).
  raw->view = CefBrowserView::CreateBrowserView(
      raw->client, "about:blank", settings, extra_info,
      ContextFor(options.partition), new ChildViewDelegate(id));
  if (!raw->view) {
    Views().erase(id);
    WebViewSinks::Instance().Abandon(id);
    *status_out = LAUFEY_WEBVIEW_FAILED;
    return 0;
  }
  raw->overlay =
      window->AddOverlayView(raw->view, CEF_DOCKING_MODE_CUSTOM, true);
  if (!raw->overlay) {
    raw->view = nullptr;
    Views().erase(id);
    WebViewSinks::Instance().Abandon(id);
    *status_out = LAUFEY_WEBVIEW_FAILED;
    return 0;
  }
  raw->overlay->SetBounds(OverlayRect(raw));
  raw->overlay->SetVisible(options.visible);
  *status_out = LAUFEY_WEBVIEW_OK;
  return id;
}

bool SetBounds(uint32_t id, double x, double y, double width, double height) {
  ChildView* v = Find(id);
  if (!v)
    return false;
  v->bounds = {x, y, width, height};
  if (v->overlay && v->overlay->IsValid())
    v->overlay->SetBounds(OverlayRect(v));
  return true;
}

bool SetVisible(uint32_t id, bool visible) {
  ChildView* v = Find(id);
  if (!v)
    return false;
  v->visible = visible;
  if (v->overlay && v->overlay->IsValid())
    v->overlay->SetVisible(visible);
  return true;
}

int32_t Navigate(uint32_t id, const std::string& url) {
  ChildView* v = Find(id);
  if (!v)
    return LAUFEY_WEBVIEW_CLOSED;
  if (!v->browser) {
    v->pending_url = url;  // OnAfterCreated loads it
    return LAUFEY_WEBVIEW_OK;
  }
  v->browser->GetMainFrame()->LoadURL(url);
  return LAUFEY_WEBVIEW_OK;
}

bool Command(uint32_t id, int32_t command) {
  ChildView* v = Find(id);
  if (!v)
    return false;
  if (!v->browser)
    return command != LAUFEY_WEBVIEW_COMMAND_FOCUS;
  CefRefPtr<CefBrowser> b = v->browser;
  switch (command) {
    case LAUFEY_WEBVIEW_COMMAND_BACK:
      if (b->CanGoBack())
        b->GoBack();
      return true;
    case LAUFEY_WEBVIEW_COMMAND_FORWARD:
      if (b->CanGoForward())
        b->GoForward();
      return true;
    case LAUFEY_WEBVIEW_COMMAND_RELOAD:
      b->Reload();
      return true;
    case LAUFEY_WEBVIEW_COMMAND_RELOAD_IGNORE_CACHE:
      b->ReloadIgnoreCache();
      return true;
    case LAUFEY_WEBVIEW_COMMAND_STOP:
      b->StopLoad();
      return true;
    case LAUFEY_WEBVIEW_COMMAND_FOCUS:
      if (!v->visible || !v->view)
        return false;
      v->view->RequestFocus();
      b->GetHost()->SetFocus(true);
      return true;
  }
  return false;
}

std::string State(uint32_t id) {
  ChildView* v = Find(id);
  if (!v)
    return "";
  laufey_common::WebViewStateFields s;
  s.url = CurrentUrl(v);
  if (s.url == "about:blank" && !v->url_sent)
    s.url = v->pending_url;
  s.title = v->title;
  s.load = v->load;
  s.can_go_back = v->browser && v->browser->CanGoBack();
  s.can_go_forward = v->browser && v->browser->CanGoForward();
  s.visible = v->visible;
  s.bounds = v->bounds;
  s.z_order = WebViewSinks::Instance().ZOrderOf(id);
  return laufey_common::WebViewStateJson(s);
}

void Evaluate(uint32_t id, const std::string& script,
              laufey_webview_eval_fn callback, void* user_data) {
  CEF_REQUIRE_UI_THREAD();
  if (!Find(id)) {
    callback(user_data, LAUFEY_WEBVIEW_CLOSED, "the web view is closed");
    return;
  }
  auto done = [callback, user_data](int32_t status, std::string text) {
    callback(user_data, status, text.c_str());
  };
  auto fail = [id, done](CefRefPtr<CefDictionaryValue> r, const char* what) {
    done(Find(id) ? LAUFEY_WEBVIEW_FAILED : LAUFEY_WEBVIEW_CLOSED,
         ErrorMessage(r, what));
  };
  SendDevTools(
      id, "Page.getFrameTree", CefDictionaryValue::Create(),
      [id, script, done, fail](bool ok, CefRefPtr<CefDictionaryValue> r) {
        CefRefPtr<CefDictionaryValue> tree =
            ok && r->GetType("frameTree") == VTYPE_DICTIONARY
                ? r->GetDictionary("frameTree")
                : nullptr;
        CefRefPtr<CefDictionaryValue> frame =
            tree && tree->GetType("frame") == VTYPE_DICTIONARY
                ? tree->GetDictionary("frame")
                : nullptr;
        if (!frame || frame->GetType("id") != VTYPE_STRING) {
          fail(r, "the page has no frame to run the script in");
          return;
        }
        CefRefPtr<CefDictionaryValue> world = CefDictionaryValue::Create();
        world->SetString("frameId", frame->GetString("id"));
        world->SetString("worldName", "laufey-webview");
        SendDevTools(
            id, "Page.createIsolatedWorld", world,
            [id, script, done, fail](bool ok, CefRefPtr<CefDictionaryValue> r) {
              if (!ok || r->GetType("executionContextId") != VTYPE_INT) {
                fail(r, "the script's world could not be made");
                return;
              }
              int context_id = r->GetInt("executionContextId");
              CefRefPtr<CefDictionaryValue> eval = CefDictionaryValue::Create();
              eval->SetString("expression", script);
              eval->SetInt("contextId", context_id);
              eval->SetBool("awaitPromise", true);
              eval->SetBool("returnByValue", false);
              eval->SetString("objectGroup", "laufey-webview-eval");
              SendDevTools(
                  id, "Runtime.evaluate", eval,
                  [id, context_id, done, fail](
                      bool ok, CefRefPtr<CefDictionaryValue> r) {
                    if (!ok) {
                      fail(r, "the script could not be run");
                      return;
                    }
                    if (r->GetType("exceptionDetails") == VTYPE_DICTIONARY) {
                      done(LAUFEY_WEBVIEW_SCRIPT_ERROR,
                           ExceptionText(r->GetDictionary("exceptionDetails")));
                      return;
                    }
                    if (r->GetType("result") != VTYPE_DICTIONARY) {
                      fail(r, "the script's result could not be read");
                      return;
                    }
                    EvalSerialize(id, context_id, r->GetDictionary("result"),
                                  done);
                  });
            });
      });
}

namespace {
void CaptureWith(uint32_t id, CefRefPtr<CefDictionaryValue> params,
                 laufey_webview_capture_fn callback, void* user_data) {
  SendDevTools(
      id, "Page.captureScreenshot", params,
      [id, callback, user_data](bool ok, CefRefPtr<CefDictionaryValue> r) {
        if (!ok || r->GetType("data") != VTYPE_STRING) {
          std::string message =
              ErrorMessage(r, "the page could not be captured");
          callback(user_data,
                   Find(id) ? LAUFEY_WEBVIEW_FAILED : LAUFEY_WEBVIEW_CLOSED,
                   nullptr, 0, message.c_str());
          return;
        }
        CefRefPtr<CefBinaryValue> bytes = CefBase64Decode(r->GetString("data"));
        size_t size = bytes ? bytes->GetSize() : 0;
        if (!size) {
          callback(user_data, LAUFEY_WEBVIEW_FAILED, nullptr, 0,
                   "the image could not be decoded");
          return;
        }
        std::vector<uint8_t> data(size);
        bytes->GetData(data.data(), size, 0);
        callback(user_data, LAUFEY_WEBVIEW_OK, data.data(), data.size(),
                 nullptr);
      });
}
}  // namespace

void Capture(uint32_t id, int32_t format, int32_t quality, double scale,
             laufey_webview_capture_fn callback, void* user_data) {
  CEF_REQUIRE_UI_THREAD();
  if (!Find(id)) {
    callback(user_data, LAUFEY_WEBVIEW_CLOSED, nullptr, 0,
             "the web view is closed");
    return;
  }
  CefRefPtr<CefDictionaryValue> params = CefDictionaryValue::Create();
  bool jpeg = format == LAUFEY_WEBVIEW_CAPTURE_JPEG;
  params->SetString("format", jpeg ? "jpeg" : "png");
  if (jpeg)
    params->SetInt("quality", quality);
  params->SetBool("fromSurface", true);
  if (!(scale > 0)) {
    CaptureWith(id, params, callback, user_data);
    return;
  }
  // A scale: clip to the visual viewport at scale / devicePixelRatio (the
  // screenshot's pixels are clip CSS px x clip.scale x the device scale).
  SendDevTools(
      id, "Page.getLayoutMetrics", CefDictionaryValue::Create(),
      [id, params, scale, callback, user_data](
          bool ok, CefRefPtr<CefDictionaryValue> r) {
        auto viewport = [&](const char* key) {
          return ok && r->GetType(key) == VTYPE_DICTIONARY
                     ? r->GetDictionary(key)
                     : nullptr;
        };
        auto number = [](CefRefPtr<CefDictionaryValue> d, const char* key) {
          if (!d)
            return 0.0;
          if (d->GetType(key) == VTYPE_DOUBLE)
            return d->GetDouble(key);
          if (d->GetType(key) == VTYPE_INT)
            return static_cast<double>(d->GetInt(key));
          return 0.0;
        };
        CefRefPtr<CefDictionaryValue> css = viewport("cssVisualViewport");
        CefRefPtr<CefDictionaryValue> device = viewport("visualViewport");
        double css_width = number(css, "clientWidth");
        double dpr =
            css_width > 0 ? number(device, "clientWidth") / css_width : 0;
        if (css && css_width > 0 && number(css, "clientHeight") > 0) {
          CefRefPtr<CefDictionaryValue> clip = CefDictionaryValue::Create();
          clip->SetDouble("x", number(css, "pageX"));
          clip->SetDouble("y", number(css, "pageY"));
          clip->SetDouble("width", css_width);
          clip->SetDouble("height", number(css, "clientHeight"));
          clip->SetDouble("scale", dpr > 0 ? scale / dpr : scale);
          params->SetDictionary("clip", clip);
        }
        CaptureWith(id, params, callback, user_data);
      });
}

bool Close(uint32_t id) {
  CEF_REQUIRE_UI_THREAD();
  ChildView* v = Find(id);
  if (!v)
    return false;
  v->closing = true;
  FailPending(v, "the web view is closed");
  WebViewSinks::Instance().Close(id);
  // Dropping the overlay and the last reference to the browser view closes
  // the browser (cefclient's overlay browser does the same); OnBeforeClose
  // then forgets the record.
  if (v->overlay && v->overlay->IsValid())
    v->overlay->Destroy();
  v->overlay = nullptr;
  v->page_view = nullptr;
  if (v->browser)
    v->browser->GetHost()->CloseBrowser(true);
  v->view = nullptr;
  if (!v->browser)
    Views().erase(id);  // never created: no OnBeforeClose will come
  return true;
}

void CloseAllForWindow(uint32_t window_id) {
  CEF_REQUIRE_UI_THREAD();
  std::vector<uint32_t> ids;
  for (const auto& kv : Views()) {
    if (kv.second->window_id == window_id && !kv.second->closing)
      ids.push_back(kv.first);
  }
  for (uint32_t id : ids)
    Close(id);
}

// TODO(api58): zoom, find, DevTools and print (stubs until implemented).
bool SetZoom(uint32_t, double) {
  return false;
}
double GetZoom(uint32_t) {
  return 0;
}
bool Find(uint32_t, const std::string&, uint32_t) {
  return false;
}
bool StopFind(uint32_t) {
  return false;
}
int32_t OpenDevTools(uint32_t) {
  return LAUFEY_WEBVIEW_UNSUPPORTED;
}
bool Print(uint32_t) {
  return false;
}

}  // namespace laufey_cef_child_webview
