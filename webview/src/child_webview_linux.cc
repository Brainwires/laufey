// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Child web views on WebKitGTK (API 58); see child_webview_linux.h and
// docs/webviews.md.
//
// Each view is its own WebKitWebView in a WebKitWebContext of its own (one
// per partition name, over a WebKitWebsiteDataManager: persistent under
// <app data dir>/WebViews/<name>, or ephemeral), never the page's context,
// with a user content manager of its own: no "laufey" script message
// handler, no init script, no URI scheme handlers. It is an overlay child of
// the GtkOverlay that holds the window's page view, so it draws above the
// page, placed by the overlay's get-child-position at the view's bounds; the
// window's navigation guard and will-navigate handler are the page view's
// signal handlers and never see it.

#include "child_webview_linux.h"

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "laufey_backend_common.h"
#include "laufey_launch_config.h"
#include "laufey_navigation_guard.h"

using laufey_common::WebViewOptions;
using laufey_common::WebViewSinks;

namespace laufey_child_webview_linux {
namespace {

// call_async_javascript_function (with a script world) is WebKitGTK 2.40.
#if WEBKIT_CHECK_VERSION(2, 40, 0)
#define LAUFEY_CHILD_WEBVIEW_EVALUATE 1
#else
#define LAUFEY_CHILD_WEBVIEW_EVALUATE 0
#endif

struct ChildView {
  uint32_t id = 0;
  uint32_t window_id = 0;
  // Strong references, dropped by Close.
  WebKitWebView* view = nullptr;
  WebKitUserContentManager* content_manager = nullptr;
  GtkWidget* overlay = nullptr;
  // Cancelled by Close, so a pending evaluate / capture answers once.
  GCancellable* cancellable = nullptr;
  WebViewOptions options;
  const char* load = "complete";
  // The current navigation already ended (failed, stopped or replaced), so
  // its FINISHED sends no "complete".
  bool load_ended = false;
  laufey_common::WebViewBounds bounds;
  bool visible = true;
  bool focused = false;
  // The last URL event, so notify::uri and the back-forward list's changes
  // of one navigation send it once.
  std::string sent_url;
  bool sent_back = false;
  bool sent_forward = false;
  bool url_flush_pending = false;
  // Console events this second (a page can't flood the embedder).
  int64_t console_second = 0;
  int console_count = 0;
  // The page world's last window.open() (the shim's "open" message): WebKitGTK
  // reports a popup's window features only to the view it would create.
  std::string open_url;
  bool open_popup = false;
  // Permission prompts this view has open (closed with it).
  std::vector<GtkWidget*> dialogs;
};

// GTK thread only.
std::map<uint32_t, ChildView>& Views() {
  static auto* views = new std::map<uint32_t, ChildView>();
  return *views;
}

ChildView* Find(uint32_t id) {
  auto it = Views().find(id);
  return it == Views().end() ? nullptr : &it->second;
}

ChildView* FindByWidget(GtkWidget* widget) {
  for (auto& kv : Views()) {
    if (GTK_WIDGET(kv.second.view) == widget)
      return &kv.second;
  }
  return nullptr;
}

gpointer IdData(uint32_t id) {
  return GUINT_TO_POINTER(id);
}

uint32_t DataId(gpointer data) {
  return GPOINTER_TO_UINT(data);
}

std::string Str(const char* s) {
  return s ? std::string(s) : std::string();
}

std::string CurrentUrl(ChildView* v) {
  return Str(webkit_web_view_get_uri(v->view));
}

constexpr int kConsolePerSecond = 200;
constexpr size_t kConsoleMaxBytes = 16 * 1024;
constexpr const char* kConsoleHandler = "__laufeyWebViewConsole";
constexpr const char* kEvalWorld = "laufey-webview";

// Page-world shim: WebKitGTK reports no console messages to the embedder.
// The page can see (and post to) this handler; it carries console text, and
// the URL and geometry of a window.open() call, which only pick the
// disposition of a refused popup's NEW_WINDOW event.
constexpr const char kConsoleShim[] =
    "(() => {"
    " const h = window.webkit && window.webkit.messageHandlers &&"
    "  window.webkit.messageHandlers.__laufeyWebViewConsole;"
    " if (!h) return;"
    " const post = h.postMessage.bind(h);"
    " const text = (args) => Array.prototype.map.call(args, (a) => {"
    "  if (typeof a === 'string') return a;"
    "  try { const s = JSON.stringify(a); return s === undefined ? String(a) "
    ": s; }"
    "  catch (_) { return String(a); } }).join(' ');"
    " for (const level of ['log', 'info', 'warn', 'error', 'debug']) {"
    "  const orig = console[level];"
    "  console[level] = function (...args) {"
    "   try { post({ level, message: text(args) }); } catch (_) {}"
    "   return orig.apply(this, args);"
    "  };"
    " }"
    " window.addEventListener('error', (e) => {"
    "  try { post({ level: 'error', message: String(e.message),"
    "   source: String(e.filename || ''), line: e.lineno | 0 }); } catch (_) "
    "{}"
    " });"
    " const open = window.open;"
    " window.open = function (url, target, features) {"
    "  try {"
    "   const href = url === undefined || url === null || url === '' ?"
    "    'about:blank' : new URL(String(url), document.baseURI).href;"
    "   post({ kind: 'open', url: href, popup:"
    "    /(^|[\\s,])(width|height|left|top|screenx|screeny)\\s*=/i"
    "     .test(String(features || '')) });"
    "  } catch (_) {}"
    "  return open.apply(this, arguments);"
    " };"
    "})();";

void Emit(uint32_t id, int32_t kind, const std::string& detail) {
  WebViewSinks::Instance().Emit(id, kind, detail);
}

void SetLoad(ChildView* v, const char* state, const std::string& url,
             const char* code = nullptr, const std::string& message = "") {
  v->load = state;
  Emit(v->id, LAUFEY_WEBVIEW_EVENT_LOAD,
       laufey_common::WebViewLoadJson(state, url, code, message));
}

// Sends the URL event when the URL or the history edges changed; notify::uri
// and the back-forward list report one navigation in pieces, so it waits for
// the burst.
void ScheduleUrlEvent(uint32_t id) {
  ChildView* v = Find(id);
  if (!v || v->url_flush_pending)
    return;
  v->url_flush_pending = true;
  g_idle_add(
      [](gpointer data) -> gboolean {
        uint32_t view_id = DataId(data);
        ChildView* view = Find(view_id);
        if (!view)
          return G_SOURCE_REMOVE;
        view->url_flush_pending = false;
        std::string url = CurrentUrl(view);
        bool back = webkit_web_view_can_go_back(view->view);
        bool forward = webkit_web_view_can_go_forward(view->view);
        if (url == view->sent_url && back == view->sent_back &&
            forward == view->sent_forward)
          return G_SOURCE_REMOVE;
        view->sent_url = url;
        view->sent_back = back;
        view->sent_forward = forward;
        Emit(view_id, LAUFEY_WEBVIEW_EVENT_URL,
             laufey_common::WebViewUrlJson(url, back, forward));
        return G_SOURCE_REMOVE;
      },
      IdData(id));
}

void OpenInBrowser(const std::string& url) {
  if (!laufey_common::IsAllowedWebViewUrl(url))
    return;
  if (laufey_common::ExternalOpenCapture::Instance().Capture(url))
    return;
  g_app_info_launch_default_for_uri(url.c_str(), nullptr, nullptr);
}

// The engine-neutral error code of a failed load, or nullptr for a load that
// ended without failing (a stop, a navigation replaced by another, a
// download, a navigation this view refused).
const char* LoadErrorCode(const GError* error) {
  if (!error)
    return "failed";
  if (error->domain == WEBKIT_NETWORK_ERROR) {
    switch (error->code) {
      case WEBKIT_NETWORK_ERROR_CANCELLED:
        return nullptr;
      case WEBKIT_NETWORK_ERROR_TRANSPORT:
        return "network";
      case WEBKIT_NETWORK_ERROR_UNKNOWN_PROTOCOL:
        return "blocked";
      default:
        return "failed";
    }
  }
  if (error->domain == WEBKIT_POLICY_ERROR) {
    switch (error->code) {
      case WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE:
        return nullptr;
      case WEBKIT_POLICY_ERROR_CANNOT_SHOW_URI:
      case WEBKIT_POLICY_ERROR_CANNOT_USE_RESTRICTED_PORT:
        return "blocked";
      default:
        return "failed";
    }
  }
  if (error->domain == WEBKIT_PLUGIN_ERROR &&
      error->code == WEBKIT_PLUGIN_ERROR_WILL_HANDLE_LOAD)
    return nullptr;
  // libsoup's (GIO's) own errors keep their domains.
  if (error->domain == G_RESOLVER_ERROR)
    return "dns";
  if (error->domain == G_TLS_ERROR)
    return "tls";
  if (error->domain == G_IO_ERROR) {
    switch (error->code) {
      case G_IO_ERROR_CANCELLED:
        return nullptr;
      case G_IO_ERROR_HOST_NOT_FOUND:
        return "dns";
      case G_IO_ERROR_CONNECTION_REFUSED:
      case G_IO_ERROR_TIMED_OUT:
      case G_IO_ERROR_HOST_UNREACHABLE:
      case G_IO_ERROR_NETWORK_UNREACHABLE:
      case G_IO_ERROR_BROKEN_PIPE:  // also G_IO_ERROR_CONNECTION_CLOSED
      case G_IO_ERROR_PROXY_FAILED:
      case G_IO_ERROR_PROXY_AUTH_FAILED:
      case G_IO_ERROR_PROXY_NEED_AUTH:
      case G_IO_ERROR_PROXY_NOT_ALLOWED:
        return "network";
      default:
        return "failed";
    }
  }
  return "failed";
}

// --- Partitions
// ---------------------------------------------------------------

void OnDownloadStarted(WebKitWebContext* context, WebKitDownload* download,
                       gpointer data);

// Contexts by partition name: one WebKitWebContext (over its own website
// data manager) per name, so views with the same partition share cookies and
// storage. Kept for the life of the process.
std::map<std::string, WebKitWebContext*>& Contexts() {
  static auto* contexts = new std::map<std::string, WebKitWebContext*>();
  return *contexts;
}

// Persistent partitions live under the app's data directory; without one
// (no LAUFEY_DATA_DIR / LAUFEY_APP_ID) every partition is in memory.
bool HasPersistentRoot() {
  return !laufey_common::AppDataDir().empty();
}

WebKitWebsiteDataManager* PersistentManager(const std::string& partition) {
  if (!HasPersistentRoot())
    return nullptr;
  std::string root = laufey_common::AppDataSubdir("WebViews");
  if (root.empty())
    return nullptr;
  std::string dir =
      laufey_common::JoinPath(root, laufey_common::PartitionDirName(partition));
  std::string data_dir = laufey_common::JoinPath(dir, "data");
  std::string cache_dir = laufey_common::JoinPath(dir, "cache");
  if (!laufey_common::EnsureDirectory(data_dir) ||
      !laufey_common::EnsureDirectory(cache_dir)) {
    g_warning(
        "laufey: could not create the web view partition directory "
        "\"%s\"; the partition is kept in memory",
        dir.c_str());
    return nullptr;
  }
  WebKitWebsiteDataManager* manager = webkit_website_data_manager_new(
      "base-data-directory", data_dir.c_str(), "base-cache-directory",
      cache_dir.c_str(), nullptr);
  std::string cookies = laufey_common::JoinPath(data_dir, "cookies.sqlite");
  webkit_cookie_manager_set_persistent_storage(
      webkit_website_data_manager_get_cookie_manager(manager), cookies.c_str(),
      WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
  return manager;
}

WebKitWebContext* ContextFor(const std::string& partition) {
  auto it = Contexts().find(partition);
  if (it != Contexts().end())
    return it->second;
  WebKitWebsiteDataManager* manager = nullptr;
  if (!laufey_common::IsEphemeralPartition(partition))
    manager = PersistentManager(partition);
  if (!manager)
    manager = webkit_website_data_manager_new_ephemeral();
  WebKitWebContext* context =
      webkit_web_context_new_with_website_data_manager(manager);
  g_object_unref(manager);  // the context keeps its own reference
  // No URI scheme is registered on it, and no web extension is loaded.
  g_signal_connect(context, "download-started", G_CALLBACK(OnDownloadStarted),
                   nullptr);
  Contexts()[partition] = context;
  return context;
}

// --- Placement
// ------------------------------------------------------------------

constexpr double kMaxCoordinate = 32767;  // GDK / X11 window geometry limit

int ClampCoordinate(double value, double min) {
  return static_cast<int>(std::lround(std::clamp(value, min, kMaxCoordinate)));
}

// The overlay's get-child-position: a child view sits at its bounds, in
// logical GTK pixels (CSS pixels at zoom 1) from the page's top-left corner.
gboolean OnGetChildPosition(GtkOverlay* overlay, GtkWidget* widget,
                            GdkRectangle* allocation, gpointer data) {
  ChildView* v = FindByWidget(widget);
  if (!v)
    return FALSE;  // not a web view: GTK's own placement
  // GTK wants a size request before an allocation.
  GtkRequisition min;
  gtk_widget_get_preferred_size(widget, &min, nullptr);
  allocation->x = ClampCoordinate(v->bounds.x, -kMaxCoordinate);
  allocation->y = ClampCoordinate(v->bounds.y, -kMaxCoordinate);
  allocation->width = ClampCoordinate(v->bounds.width, 0);
  allocation->height = ClampCoordinate(v->bounds.height, 0);
  return TRUE;
}

void HookOverlay(GtkWidget* overlay) {
  static const char kHooked[] = "laufey-webview-overlay";
  if (g_object_get_data(G_OBJECT(overlay), kHooked))
    return;
  g_object_set_data(G_OBJECT(overlay), kHooked, GINT_TO_POINTER(1));
  g_signal_connect(overlay, "get-child-position",
                   G_CALLBACK(OnGetChildPosition), nullptr);
}

void NoteFocus(uint32_t id, bool focused) {
  ChildView* v = Find(id);
  if (!v || v->focused == focused)
    return;
  v->focused = focused;
  Emit(id, LAUFEY_WEBVIEW_EVENT_FOCUS,
       laufey_common::WebViewFocusJson(focused));
}

// --- Popups
// -----------------------------------------------------------------------

// A popup the page asked for (window.open, a target=_blank link, the
// context menu's "open in new window"): nothing new is created; the URL
// loads here, opens in the browser, or is reported, as the view's
// allowPopups says.
void HandlePopup(ChildView* v, WebKitNavigationAction* action) {
  WebKitURIRequest* request =
      action ? webkit_navigation_action_get_request(action) : nullptr;
  std::string url =
      Str(request ? webkit_uri_request_get_uri(request) : nullptr);
  if (url.empty())
    url = "about:blank";
  bool popup = !v->open_url.empty() && v->open_url == url && v->open_popup;
  v->open_url.clear();
  v->open_popup = false;
  switch (v->options.popups) {
    case laufey_common::WebViewPopups::kSameView:
      if (laufey_common::IsAllowedWebViewUrl(url))
        webkit_web_view_load_uri(v->view, url.c_str());
      break;
    case laufey_common::WebViewPopups::kBrowser:
      OpenInBrowser(url);
      break;
    case laufey_common::WebViewPopups::kDeny:
      Emit(v->id, LAUFEY_WEBVIEW_EVENT_NEW_WINDOW,
           laufey_common::WebViewNewWindowJson(url, popup));
      break;
  }
}

GtkWidget* OnCreate(WebKitWebView* web_view, WebKitNavigationAction* action,
                    gpointer data) {
  if (ChildView* v = Find(DataId(data)))
    HandlePopup(v, action);
  return nullptr;  // no second web view: the page's window.open() gets null
}

// --- Downloads
// ----------------------------------------------------------------------

bool IsAttachment(WebKitURIResponse* response) {
  SoupMessageHeaders* headers =
      response ? webkit_uri_response_get_http_headers(response) : nullptr;
  const char* value =
      headers ? soup_message_headers_get_one(headers, "Content-Disposition")
              : nullptr;
  return value && g_ascii_strncasecmp(value, "attachment", 10) == 0;
}

std::string DownloadUrl(WebKitDownload* download) {
  WebKitURIResponse* response = webkit_download_get_response(download);
  const char* uri = response ? webkit_uri_response_get_uri(response) : nullptr;
  if (!uri) {
    WebKitURIRequest* request = webkit_download_get_request(download);
    uri = request ? webkit_uri_request_get_uri(request) : nullptr;
  }
  return Str(uri);
}

#if WEBKIT_CHECK_VERSION(2, 40, 0)
void OnSaveResponse(GtkNativeDialog* dialog, gint response, gpointer data) {
  auto* download = static_cast<WebKitDownload*>(data);
  gchar* path = response == GTK_RESPONSE_ACCEPT
                    ? gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog))
                    : nullptr;
  if (path) {
    webkit_download_set_destination(download, path);
    // The save dialog asked before replacing a file; WebKit refuses to
    // replace one unless told.
    webkit_download_set_allow_overwrite(download, TRUE);
    g_free(path);
  } else {
    webkit_download_cancel(download);
  }
  g_object_unref(download);
  g_object_unref(dialog);
}
#endif

// decide-destination of a download "downloads": "ask" let through: the OS
// save dialog. Asynchronous (WebKitGTK 2.40+: returning TRUE without a
// destination holds the download until one is set or it is cancelled).
gboolean OnDecideDestination(WebKitDownload* download,
                             const gchar* suggested_filename, gpointer data) {
#if WEBKIT_CHECK_VERSION(2, 40, 0)
  ChildView* v = Find(DataId(data));
  GtkWidget* toplevel =
      v ? gtk_widget_get_toplevel(GTK_WIDGET(v->view)) : nullptr;
  GtkWindow* parent = toplevel && gtk_widget_is_toplevel(toplevel)
                          ? GTK_WINDOW(toplevel)
                          : nullptr;
  GtkFileChooserNative* chooser = gtk_file_chooser_native_new(
      nullptr, parent, GTK_FILE_CHOOSER_ACTION_SAVE, nullptr, nullptr);
  gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(chooser),
                                                 TRUE);
  gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser),
                                    suggested_filename && *suggested_filename
                                        ? suggested_filename
                                        : "download");
  gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(chooser), parent != nullptr);
  g_object_ref(download);
  g_signal_connect(chooser, "response", G_CALLBACK(OnSaveResponse), download);
  gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
#else
  // An older WebKitGTK can't wait for the dialog's answer: refuse rather
  // than save unasked.
  webkit_download_cancel(download);
#endif
  return TRUE;
}

// Every download of a web view's context: a response the view could not
// show that "ask" let through (webkit_policy_decision_download), or an
// <a download> link, which WebKit turns into a download on its own.
void OnDownloadStarted(WebKitWebContext* context, WebKitDownload* download,
                       gpointer data) {
  WebKitWebView* web_view = webkit_download_get_web_view(download);
  ChildView* v = web_view ? FindByWidget(GTK_WIDGET(web_view)) : nullptr;
  if (!v) {
    webkit_download_cancel(download);
    return;
  }
  switch (v->options.downloads) {
    case laufey_common::WebViewDownloads::kAsk:
      g_signal_connect(download, "decide-destination",
                       G_CALLBACK(OnDecideDestination), IdData(v->id));
      return;
    case laufey_common::WebViewDownloads::kBrowser: {
      std::string url = DownloadUrl(download);
      webkit_download_cancel(download);
      OpenInBrowser(url);
      return;
    }
    case laufey_common::WebViewDownloads::kDeny:
      webkit_download_cancel(download);
      return;
  }
}

// --- Permissions
// --------------------------------------------------------------------

// What a prompt calls the permission, or nullptr for one that is refused
// without asking.
const char* PermissionName(WebKitPermissionRequest* request) {
  if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(request))
    return "your location";
  if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(request))
    return "notifications";
  if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(request)) {
    auto* media = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(request);
#if WEBKIT_CHECK_VERSION(2, 34, 0)
    if (webkit_user_media_permission_is_for_display_device(media))
      return "your screen";
#endif
    bool audio = webkit_user_media_permission_is_for_audio_device(media);
    bool video = webkit_user_media_permission_is_for_video_device(media);
    if (audio && video)
      return "your camera and microphone";
    if (video)
      return "your camera";
    if (audio)
      return "your microphone";
    return nullptr;
  }
#if WEBKIT_CHECK_VERSION(2, 28, 0)
  if (WEBKIT_IS_POINTER_LOCK_PERMISSION_REQUEST(request))
    return "pointer lock";
#endif
#if WEBKIT_CHECK_VERSION(2, 30, 0)
  if (WEBKIT_IS_WEBSITE_DATA_ACCESS_PERMISSION_REQUEST(request))
    return "its cookies and site data while embedded";
#endif
  return nullptr;
}

struct PermissionAsk {
  WebKitPermissionRequest* request;
  bool decided = false;
};

void FreePermissionAsk(gpointer data) {
  auto* ask = static_cast<PermissionAsk*>(data);
  if (!ask->decided)
    webkit_permission_request_deny(ask->request);
  g_object_unref(ask->request);
  delete ask;
}

constexpr const char* kPermissionAskKey = "laufey-webview-permission";

void OnPermissionResponse(GtkDialog* dialog, gint response, gpointer) {
  auto* ask = static_cast<PermissionAsk*>(
      g_object_get_data(G_OBJECT(dialog), kPermissionAskKey));
  if (ask && !ask->decided) {
    ask->decided = true;
    if (response == GTK_RESPONSE_ACCEPT)
      webkit_permission_request_allow(ask->request);
    else
      webkit_permission_request_deny(ask->request);
  }
  gtk_widget_destroy(GTK_WIDGET(dialog));
}

void OnPermissionDialogDestroy(GtkWidget* dialog, gpointer data) {
  if (ChildView* v = Find(DataId(data))) {
    auto& d = v->dialogs;
    d.erase(std::remove(d.begin(), d.end(), dialog), d.end());
  }
}

std::string OriginOf(const std::string& url) {
  WebKitSecurityOrigin* origin =
      webkit_security_origin_new_for_uri(url.c_str());
  if (!origin)
    return url;
  gchar* text = webkit_security_origin_to_string(origin);
  std::string result = text ? text : url;
  g_free(text);
  webkit_security_origin_unref(origin);
  return result;
}

gboolean OnPermissionRequest(WebKitWebView* web_view,
                             WebKitPermissionRequest* request, gpointer data) {
  ChildView* v = Find(DataId(data));
  const char* what = PermissionName(request);
  if (!v || !what ||
      v->options.permissions != laufey_common::WebViewPermissions::kAsk) {
    webkit_permission_request_deny(request);
    return TRUE;
  }
  GtkWidget* toplevel = gtk_widget_get_toplevel(GTK_WIDGET(web_view));
  GtkWindow* parent = toplevel && gtk_widget_is_toplevel(toplevel)
                          ? GTK_WINDOW(toplevel)
                          : nullptr;
  std::string origin = OriginOf(CurrentUrl(v));
  GtkWidget* dialog = gtk_message_dialog_new(
      parent, GTK_DIALOG_DESTROY_WITH_PARENT, GTK_MESSAGE_QUESTION,
      GTK_BUTTONS_NONE, "%s wants to use %s", origin.c_str(), what);
  gtk_dialog_add_buttons(GTK_DIALOG(dialog), "_Block", GTK_RESPONSE_REJECT,
                         "_Allow", GTK_RESPONSE_ACCEPT, nullptr);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_REJECT);
  gtk_window_set_modal(GTK_WINDOW(dialog), parent != nullptr);
  auto* ask = new PermissionAsk{
      WEBKIT_PERMISSION_REQUEST(g_object_ref(request)), false};
  // Undecided when the dialog goes (the view closed): refused.
  g_object_set_data_full(G_OBJECT(dialog), kPermissionAskKey, ask,
                         FreePermissionAsk);
  g_signal_connect(dialog, "response", G_CALLBACK(OnPermissionResponse),
                   nullptr);
  g_signal_connect(dialog, "destroy", G_CALLBACK(OnPermissionDialogDestroy),
                   IdData(v->id));
  v->dialogs.push_back(dialog);
  gtk_widget_show(dialog);
  return TRUE;
}

// --- Navigation
// -----------------------------------------------------------------------

gboolean OnDecidePolicy(WebKitWebView* web_view, WebKitPolicyDecision* decision,
                        WebKitPolicyDecisionType type, gpointer data) {
  ChildView* v = Find(DataId(data));
  switch (type) {
    case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION: {
      WebKitNavigationAction* action =
          webkit_navigation_policy_decision_get_navigation_action(
              WEBKIT_NAVIGATION_POLICY_DECISION(decision));
      WebKitURIRequest* request =
          action ? webkit_navigation_action_get_request(action) : nullptr;
      std::string url =
          Str(request ? webkit_uri_request_get_uri(request) : nullptr);
      if (!v || !laufey_common::IsAllowedWebViewNavigation(url))
        webkit_policy_decision_ignore(decision);
      else
        webkit_policy_decision_use(decision);
      return TRUE;
    }
    case WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION: {
      if (v) {
        HandlePopup(v, webkit_navigation_policy_decision_get_navigation_action(
                           WEBKIT_NAVIGATION_POLICY_DECISION(decision)));
      }
      // Never "create": no second web view.
      webkit_policy_decision_ignore(decision);
      return TRUE;
    }
    case WEBKIT_POLICY_DECISION_TYPE_RESPONSE: {
      auto* response_decision = WEBKIT_RESPONSE_POLICY_DECISION(decision);
      WebKitURIResponse* response =
          webkit_response_policy_decision_get_response(response_decision);
      if (webkit_response_policy_decision_is_mime_type_supported(
              response_decision) &&
          !IsAttachment(response))
        return FALSE;  // shown: WebKit's default
      std::string url =
          Str(response ? webkit_uri_response_get_uri(response) : nullptr);
      auto downloads =
          v ? v->options.downloads : laufey_common::WebViewDownloads::kDeny;
      switch (downloads) {
        case laufey_common::WebViewDownloads::kAsk:
          webkit_policy_decision_download(decision);
          break;
        case laufey_common::WebViewDownloads::kBrowser:
          webkit_policy_decision_ignore(decision);
          OpenInBrowser(url);
          break;
        case laufey_common::WebViewDownloads::kDeny:
          webkit_policy_decision_ignore(decision);
          break;
      }
      return TRUE;
    }
    default:
      return FALSE;
  }
}

void OnLoadChanged(WebKitWebView* web_view, WebKitLoadEvent event,
                   gpointer data) {
  ChildView* v = Find(DataId(data));
  if (!v)
    return;
  switch (event) {
    case WEBKIT_LOAD_STARTED:
      v->load_ended = false;
      SetLoad(v, "loading", CurrentUrl(v));
      break;
    case WEBKIT_LOAD_REDIRECTED:
      break;
    case WEBKIT_LOAD_COMMITTED:
      SetLoad(v, "interactive", CurrentUrl(v));
      break;
    case WEBKIT_LOAD_FINISHED:
      if (v->load_ended) {
        // Failed (already reported), or stopped / replaced: no "complete".
        v->load_ended = false;
        if (std::strcmp(v->load, "failed") != 0 &&
            !webkit_web_view_is_loading(web_view))
          v->load = "complete";
        break;
      }
      SetLoad(v, "complete", CurrentUrl(v));
      break;
  }
}

gboolean OnLoadFailed(WebKitWebView* web_view, WebKitLoadEvent event,
                      gchar* failing_uri, GError* error, gpointer data) {
  ChildView* v = Find(DataId(data));
  if (!v)
    return TRUE;
  v->load_ended = true;
  const char* code = LoadErrorCode(error);
  if (code) {
    std::string url = failing_uri ? failing_uri : CurrentUrl(v);
    SetLoad(v, "failed", url, code, Str(error ? error->message : nullptr));
  }
  // No WebKit error page: the embedder has the event.
  return TRUE;
}

gboolean OnLoadFailedWithTlsErrors(WebKitWebView* web_view, gchar* failing_uri,
                                   GTlsCertificate* certificate,
                                   GTlsCertificateFlags errors, gpointer data) {
  ChildView* v = Find(DataId(data));
  if (!v)
    return TRUE;
  v->load_ended = true;
  std::string url = failing_uri ? failing_uri : CurrentUrl(v);
  SetLoad(v, "failed", url, "tls", "the site's certificate is not trusted");
  return TRUE;  // handled: no load-failed follows
}

void OnWebProcessTerminated(WebKitWebView* web_view,
                            WebKitWebProcessTerminationReason reason,
                            gpointer data) {
  if (ChildView* v = Find(DataId(data)))
    SetLoad(v, "failed", CurrentUrl(v), "failed",
            "the page's web content process ended");
}

void OnTitle(GObject* object, GParamSpec*, gpointer data) {
  ChildView* v = Find(DataId(data));
  if (!v)
    return;
  Emit(
      v->id, LAUFEY_WEBVIEW_EVENT_TITLE,
      laufey_common::WebViewTitleJson(Str(webkit_web_view_get_title(v->view))));
}

void OnUri(GObject* object, GParamSpec*, gpointer data) {
  ScheduleUrlEvent(DataId(data));
}

void OnBackForwardChanged(WebKitBackForwardList* list,
                          WebKitBackForwardListItem* added, gpointer removed,
                          gpointer data) {
  ScheduleUrlEvent(DataId(data));
}

gboolean OnFocusIn(GtkWidget* widget, GdkEventFocus*, gpointer data) {
  NoteFocus(DataId(data), true);
  return FALSE;
}

gboolean OnFocusOut(GtkWidget* widget, GdkEventFocus*, gpointer data) {
  NoteFocus(DataId(data), false);
  return FALSE;
}

// --- Console shim
// -------------------------------------------------------------------

std::string StringProperty(JSCValue* object, const char* name) {
  JSCValue* value = jsc_value_object_get_property(object, name);
  std::string result;
  if (value && jsc_value_is_string(value)) {
    gchar* text = jsc_value_to_string(value);
    result = Str(text);
    g_free(text);
  }
  if (value)
    g_object_unref(value);
  return result;
}

void OnConsoleMessage(WebKitUserContentManager* manager,
                      WebKitJavascriptResult* result, gpointer data) {
  ChildView* v = Find(DataId(data));
  if (!v)
    return;
  JSCValue* body = webkit_javascript_result_get_js_value(result);
  if (!body || !jsc_value_is_object(body))
    return;
  int64_t second = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  if (second != v->console_second) {
    v->console_second = second;
    v->console_count = 0;
  }
  if (++v->console_count > kConsolePerSecond)
    return;
  if (StringProperty(body, "kind") == "open") {
    v->open_url = StringProperty(body, "url");
    JSCValue* popup = jsc_value_object_get_property(body, "popup");
    v->open_popup =
        popup && jsc_value_is_boolean(popup) && jsc_value_to_boolean(popup);
    if (popup)
      g_object_unref(popup);
    return;
  }
  std::string level = StringProperty(body, "level");
  std::string text = StringProperty(body, "message");
  if (text.size() > kConsoleMaxBytes)
    text.resize(kConsoleMaxBytes);
  int line_number = 0;
  JSCValue* line = jsc_value_object_get_property(body, "line");
  if (line) {
    if (jsc_value_is_number(line))
      line_number = jsc_value_to_int32(line);
    g_object_unref(line);
  }
  Emit(v->id, LAUFEY_WEBVIEW_EVENT_CONSOLE,
       laufey_common::WebViewConsoleJson(
           level.c_str(), text, StringProperty(body, "source"), line_number));
}

// --- Evaluate and capture
// ---------------------------------------------------------

bool IsCancelled(const GError* error) {
  return error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

#if LAUFEY_CHILD_WEBVIEW_EVALUATE
struct EvalCall {
  laufey_webview_eval_fn callback;
  void* user_data;
};

void OnEvalDone(GObject* source, GAsyncResult* res, gpointer data) {
  auto* call = static_cast<EvalCall*>(data);
  GError* error = nullptr;
  JSCValue* value = webkit_web_view_call_async_javascript_function_finish(
      WEBKIT_WEB_VIEW(source), res, &error);
  if (error) {
    if (IsCancelled(error)) {
      call->callback(call->user_data, LAUFEY_WEBVIEW_CLOSED,
                     "the web view is closed");
    } else {
      call->callback(call->user_data, LAUFEY_WEBVIEW_SCRIPT_ERROR,
                     error->message ? error->message : "the script failed");
    }
    g_error_free(error);
  } else if (!value || !jsc_value_is_string(value)) {
    call->callback(call->user_data, LAUFEY_WEBVIEW_SCRIPT_ERROR,
                   "the script's result could not be read");
  } else {
    gchar* envelope = jsc_value_to_string(value);
    std::string text;
    int32_t status =
        laufey_common::ParseWebViewEvalEnvelope(Str(envelope), &text);
    g_free(envelope);
    call->callback(call->user_data, status, text.c_str());
  }
  if (value)
    g_object_unref(value);
  delete call;
}
#endif

struct CaptureCall {
  laufey_webview_capture_fn callback;
  void* user_data;
  int32_t format;
  int32_t quality;
};

cairo_status_t AppendBytes(void* closure, const unsigned char* data,
                           unsigned int length) {
  auto* out = static_cast<std::vector<uint8_t>*>(closure);
  out->insert(out->end(), data, data + length);
  return CAIRO_STATUS_SUCCESS;
}

// The snapshot as PNG or JPEG, at its device pixels (WebKit scales the
// surface by the window's scale factor). Empty on failure.
std::vector<uint8_t> Encode(cairo_surface_t* surface, int32_t format,
                            int32_t quality) {
  std::vector<uint8_t> out;
  if (cairo_surface_get_type(surface) != CAIRO_SURFACE_TYPE_IMAGE)
    return out;
  cairo_surface_flush(surface);
  if (format == LAUFEY_WEBVIEW_CAPTURE_PNG) {
    if (cairo_surface_write_to_png_stream(surface, AppendBytes, &out) !=
        CAIRO_STATUS_SUCCESS)
      out.clear();
    return out;
  }
  // gdk_pixbuf_get_from_surface reads `width` x `height` in surface units:
  // read the pixels 1:1.
  cairo_surface_set_device_scale(surface, 1, 1);
  int width = cairo_image_surface_get_width(surface);
  int height = cairo_image_surface_get_height(surface);
  GdkPixbuf* pixbuf =
      width > 0 && height > 0
          ? gdk_pixbuf_get_from_surface(surface, 0, 0, width, height)
          : nullptr;
  if (!pixbuf)
    return out;
  std::string q = std::to_string(std::clamp(quality, 0, 100));
  gchar* buffer = nullptr;
  gsize size = 0;
  if (gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &size, "jpeg", nullptr,
                                "quality", q.c_str(), nullptr) &&
      buffer) {
    out.assign(reinterpret_cast<uint8_t*>(buffer),
               reinterpret_cast<uint8_t*>(buffer) + size);
  }
  g_free(buffer);
  g_object_unref(pixbuf);
  return out;
}

void OnSnapshotDone(GObject* source, GAsyncResult* res, gpointer data) {
  auto* call = static_cast<CaptureCall*>(data);
  GError* error = nullptr;
  cairo_surface_t* surface =
      webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), res, &error);
  if (!surface) {
    if (IsCancelled(error)) {
      call->callback(call->user_data, LAUFEY_WEBVIEW_CLOSED, nullptr, 0,
                     "the web view is closed");
    } else {
      std::string message =
          error && error->message ? error->message : "nothing to capture";
      call->callback(call->user_data, LAUFEY_WEBVIEW_FAILED, nullptr, 0,
                     message.c_str());
    }
    if (error)
      g_error_free(error);
    delete call;
    return;
  }
  std::vector<uint8_t> bytes = Encode(surface, call->format, call->quality);
  cairo_surface_destroy(surface);
  if (bytes.empty()) {
    call->callback(call->user_data, LAUFEY_WEBVIEW_FAILED, nullptr, 0,
                   "the image could not be encoded");
  } else {
    call->callback(call->user_data, LAUFEY_WEBVIEW_OK, bytes.data(),
                   bytes.size(), nullptr);
  }
  delete call;
}

}  // namespace

uint32_t Capabilities() {
  uint32_t caps =
      LAUFEY_WEBVIEW_CAP_SUPPORTED | LAUFEY_WEBVIEW_CAP_CAPTURE |
      LAUFEY_WEBVIEW_CAP_PERMISSION_PROMPTS |
      (LAUFEY_WEBVIEW_ENGINE_WEBKITGTK << LAUFEY_WEBVIEW_ENGINE_SHIFT);
#if LAUFEY_CHILD_WEBVIEW_EVALUATE
  caps |= LAUFEY_WEBVIEW_CAP_EVALUATE;
#endif
  if (HasPersistentRoot())
    caps |= LAUFEY_WEBVIEW_CAP_PERSISTENT_PARTITIONS;
  return caps;
}

uint32_t Create(uint32_t window_id, void* overlay_ptr,
                const WebViewOptions& options, laufey_webview_event_fn on_event,
                void* user_data, int32_t* status_out) {
  GtkWidget* overlay = static_cast<GtkWidget*>(overlay_ptr);
  if (!overlay || !GTK_IS_OVERLAY(overlay)) {
    *status_out = LAUFEY_WEBVIEW_NO_WINDOW;
    return 0;
  }
  uint32_t id = WebViewSinks::Instance().Add(window_id, on_event, user_data);
  if (!id) {
    *status_out = LAUFEY_WEBVIEW_LIMIT;
    return 0;
  }

  WebKitUserContentManager* content_manager = webkit_user_content_manager_new();
  WebKitUserScript* shim = webkit_user_script_new(
      kConsoleShim, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
  webkit_user_content_manager_add_script(content_manager, shim);
  webkit_user_script_unref(shim);
  std::string signal =
      std::string("script-message-received::") + kConsoleHandler;
  g_signal_connect(content_manager, signal.c_str(),
                   G_CALLBACK(OnConsoleMessage), IdData(id));
  webkit_user_content_manager_register_script_message_handler(content_manager,
                                                              kConsoleHandler);

  WebKitWebView* view = WEBKIT_WEB_VIEW(g_object_new(
      WEBKIT_TYPE_WEB_VIEW, "web-context", ContextFor(options.partition),
      "user-content-manager", content_manager, nullptr));
  g_object_ref_sink(view);

  WebKitSettings* settings = webkit_web_view_get_settings(view);
  if (!options.user_agent.empty())
    webkit_settings_set_user_agent(settings, options.user_agent.c_str());
  // Web Inspector follows the app's own page (LAUFEY_INSPECTABLE).
  webkit_settings_set_enable_developer_extras(
      settings, laufey_common::LaunchInspectable() ? TRUE : FALSE);
  // getUserMedia exists (WebKitGTK leaves it off); who may capture is
  // OnPermissionRequest's decision.
  webkit_settings_set_enable_media_stream(settings, TRUE);

  gpointer data = IdData(id);
  g_signal_connect(view, "decide-policy", G_CALLBACK(OnDecidePolicy), data);
  g_signal_connect(view, "create", G_CALLBACK(OnCreate), data);
  g_signal_connect(view, "load-changed", G_CALLBACK(OnLoadChanged), data);
  g_signal_connect(view, "load-failed", G_CALLBACK(OnLoadFailed), data);
  g_signal_connect(view, "load-failed-with-tls-errors",
                   G_CALLBACK(OnLoadFailedWithTlsErrors), data);
  g_signal_connect(view, "web-process-terminated",
                   G_CALLBACK(OnWebProcessTerminated), data);
  g_signal_connect(view, "permission-request", G_CALLBACK(OnPermissionRequest),
                   data);
  g_signal_connect(view, "notify::title", G_CALLBACK(OnTitle), data);
  g_signal_connect(view, "notify::uri", G_CALLBACK(OnUri), data);
  g_signal_connect(view, "focus-in-event", G_CALLBACK(OnFocusIn), data);
  g_signal_connect(view, "focus-out-event", G_CALLBACK(OnFocusOut), data);
  g_signal_connect(webkit_web_view_get_back_forward_list(view), "changed",
                   G_CALLBACK(OnBackForwardChanged), data);

  ChildView& v = Views()[id];
  v.id = id;
  v.window_id = window_id;
  v.view = view;
  v.content_manager = content_manager;
  v.overlay = GTK_WIDGET(g_object_ref(overlay));
  v.cancellable = g_cancellable_new();
  v.options = options;
  v.bounds = options.bounds;
  v.visible = options.visible;

  HookOverlay(overlay);
  // A gtk_widget_show_all of the window never reveals a hidden view.
  gtk_widget_set_no_show_all(GTK_WIDGET(view), TRUE);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), GTK_WIDGET(view));
  gtk_widget_set_visible(GTK_WIDGET(view), options.visible);

  webkit_web_view_load_uri(view, options.url.c_str());
  *status_out = LAUFEY_WEBVIEW_OK;
  return id;
}

bool SetBounds(uint32_t id, double x, double y, double width, double height) {
  ChildView* v = Find(id);
  if (!v)
    return false;
  v->bounds = {x, y, width, height};
  gtk_widget_queue_resize(GTK_WIDGET(v->view));
  return true;
}

bool SetVisible(uint32_t id, bool visible) {
  ChildView* v = Find(id);
  if (!v)
    return false;
  v->visible = visible;
  GtkWidget* widget = GTK_WIDGET(v->view);
  if (!visible && gtk_widget_has_focus(widget)) {
    // The keyboard focus goes back to the page.
    if (GtkWidget* page = gtk_bin_get_child(GTK_BIN(v->overlay)))
      gtk_widget_grab_focus(page);
  }
  gtk_widget_set_visible(widget, visible);
  return true;
}

int32_t Navigate(uint32_t id, const std::string& url) {
  ChildView* v = Find(id);
  if (!v)
    return LAUFEY_WEBVIEW_CLOSED;
  webkit_web_view_load_uri(v->view, url.c_str());
  return LAUFEY_WEBVIEW_OK;
}

bool Command(uint32_t id, int32_t command) {
  ChildView* v = Find(id);
  if (!v)
    return false;
  switch (command) {
    case LAUFEY_WEBVIEW_COMMAND_BACK:
      if (webkit_web_view_can_go_back(v->view))
        webkit_web_view_go_back(v->view);
      return true;
    case LAUFEY_WEBVIEW_COMMAND_FORWARD:
      if (webkit_web_view_can_go_forward(v->view))
        webkit_web_view_go_forward(v->view);
      return true;
    case LAUFEY_WEBVIEW_COMMAND_RELOAD:
      webkit_web_view_reload(v->view);
      return true;
    case LAUFEY_WEBVIEW_COMMAND_RELOAD_IGNORE_CACHE:
      webkit_web_view_reload_bypass_cache(v->view);
      return true;
    case LAUFEY_WEBVIEW_COMMAND_STOP:
      webkit_web_view_stop_loading(v->view);
      return true;
    case LAUFEY_WEBVIEW_COMMAND_FOCUS: {
      GtkWidget* widget = GTK_WIDGET(v->view);
      if (!v->visible || !gtk_widget_get_mapped(widget))
        return false;
      gtk_widget_grab_focus(widget);
      return true;
    }
  }
  return false;
}

std::string State(uint32_t id) {
  ChildView* v = Find(id);
  if (!v)
    return "";
  laufey_common::WebViewStateFields s;
  s.url = CurrentUrl(v);
  s.title = Str(webkit_web_view_get_title(v->view));
  s.load = v->load;
  s.can_go_back = webkit_web_view_can_go_back(v->view);
  s.can_go_forward = webkit_web_view_can_go_forward(v->view);
  s.visible = v->visible;
  s.bounds = v->bounds;
  return laufey_common::WebViewStateJson(s);
}

void Evaluate(uint32_t view_id, const std::string& script,
              laufey_webview_eval_fn callback, void* user_data) {
  ChildView* v = Find(view_id);
  if (!v) {
    callback(user_data, LAUFEY_WEBVIEW_CLOSED, "the web view is closed");
    return;
  }
#if LAUFEY_CHILD_WEBVIEW_EVALUATE
  std::string body = laufey_common::WebViewEvalScript(script);
  webkit_web_view_call_async_javascript_function(
      v->view, body.c_str(), static_cast<gssize>(body.size()), nullptr,
      kEvalWorld, nullptr, v->cancellable, OnEvalDone,
      new EvalCall{callback, user_data});
#else
  callback(user_data, LAUFEY_WEBVIEW_UNSUPPORTED,
           "evaluate needs WebKitGTK 2.40 or later");
#endif
}

void Capture(uint32_t id, int32_t format, int32_t quality,
             laufey_webview_capture_fn callback, void* user_data) {
  ChildView* v = Find(id);
  if (!v) {
    callback(user_data, LAUFEY_WEBVIEW_CLOSED, nullptr, 0,
             "the web view is closed");
    return;
  }
  webkit_web_view_get_snapshot(
      v->view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE,
      v->cancellable, OnSnapshotDone,
      new CaptureCall{callback, user_data, format, quality});
}

bool Close(uint32_t id) {
  auto it = Views().find(id);
  if (it == Views().end())
    return false;
  ChildView v = std::move(it->second);
  Views().erase(it);
  gpointer data = IdData(id);
  // A pending evaluate / capture answers CLOSED (once).
  g_cancellable_cancel(v.cancellable);
  g_object_unref(v.cancellable);
  // Its open prompts close, refused.
  std::vector<GtkWidget*> dialogs = v.dialogs;
  for (GtkWidget* dialog : dialogs)
    gtk_widget_destroy(dialog);
  g_signal_handlers_disconnect_by_data(v.view, data);
  g_signal_handlers_disconnect_by_data(
      webkit_web_view_get_back_forward_list(v.view), data);
  g_signal_handlers_disconnect_by_data(v.content_manager, data);
  webkit_user_content_manager_unregister_script_message_handler(
      v.content_manager, kConsoleHandler);
  webkit_web_view_stop_loading(v.view);
  GtkWidget* widget = GTK_WIDGET(v.view);
  if (gtk_widget_get_parent(widget) == v.overlay)
    gtk_container_remove(GTK_CONTAINER(v.overlay), widget);
  gtk_widget_destroy(widget);
  g_object_unref(v.view);
  g_object_unref(v.content_manager);
  g_object_unref(v.overlay);
  WebViewSinks::Instance().Close(id);
  return true;
}

void CloseAllForWindow(uint32_t window_id) {
  std::vector<uint32_t> ids;
  for (const auto& kv : Views()) {
    if (kv.second.window_id == window_id)
      ids.push_back(kv.first);
  }
  for (uint32_t id : ids)
    Close(id);
}

}  // namespace laufey_child_webview_linux
