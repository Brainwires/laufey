// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"

#include "laufey_backend_common.h"
#include "laufey_external_links.h"
#include "laufey_downloads.h"
#include "scheme_download.h"
#include "laufey_navigation_guard.h"
#include "laufey_user_activity.h"
#include "laufey_notifications.h"
#include "laufey_quit_request.h"
#include "laufey_media_capture.h"
#include "laufey_will_navigate.h"
#include "laufey_menu.h"
#include "laufey_platform_features.h"
#include "laufey_title_bar.h"
#if defined(__linux__) || defined(__APPLE__)
#include "laufey_secret_store.h"
#endif
#include "laufey_auth_session.h"
#include "laufey_ui_tasks.h"
#include "laufey_webview.h"
#include "laufey_window.h"

#ifndef _WIN32
#include <dlfcn.h>
#include <unistd.h>
#else
#include <windows.h>
// windows.h defines CreateWindow as a macro which conflicts with
// LaufeyBackend::CreateWindow
#undef CreateWindow
#endif

#ifdef __APPLE__
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#endif

#if defined(__linux__)
#include "laufey_io.h"
#endif

#include <iostream>
#include <cstring>
#include <vector>

RuntimeLoader* RuntimeLoader::instance_ = nullptr;

namespace {

// Absolute path of the running executable, or "" if it can't be determined.
std::string GetExecutablePath() {
#if defined(_WIN32)
  std::vector<wchar_t> buf(MAX_PATH);
  for (;;) {
    DWORD len =
        GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (len == 0)
      return "";
    if (len < buf.size()) {
      int size = WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, nullptr, 0,
                                     nullptr, nullptr);
      if (size <= 0)
        return "";
      std::string out(size - 1, '\0');
      WideCharToMultiByte(CP_UTF8, 0, buf.data(), -1, &out[0], size, nullptr,
                          nullptr);
      return out;
    }
    buf.resize(buf.size() * 2);  // truncated; grow and retry
  }
#elif defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);  // query required length
  std::vector<char> buf(size);
  if (_NSGetExecutablePath(buf.data(), &size) != 0)
    return "";
  return std::string(buf.data());
#else
  std::vector<char> buf(4096);
  ssize_t len = readlink("/proc/self/exe", buf.data(), buf.size());
  if (len <= 0)
    return "";
  return std::string(buf.data(), static_cast<size_t>(len));
#endif
}

bool PathExists(const std::string& path) {
#if defined(_WIN32)
  // Paths flow through this file as UTF-8 (see GetExecutablePath), so the
  // ANSI (*A) APIs would misread any non-ASCII characters in the active
  // codepage. Convert back to UTF-16 for the wide (*W) APIs.
  return GetFileAttributesW(laufey_common::Utf8ToWide(path).c_str()) !=
         INVALID_FILE_ATTRIBUTES;
#else
  return access(path.c_str(), F_OK) == 0;
#endif
}

}  // namespace

std::string LaufeyFindColocatedRuntime() {
  std::string exe = GetExecutablePath();
  if (exe.empty())
    return "";

  // Strip the extension from the filename component only (keep directory dots).
  size_t slash = exe.find_last_of("/\\");
  size_t file_start = (slash == std::string::npos) ? 0 : slash + 1;
  size_t dot = exe.find_last_of('.');
  std::string base =
      (dot != std::string::npos && dot > file_start) ? exe.substr(0, dot) : exe;

#if defined(_WIN32)
  std::string candidate = base + ".dll";
#elif defined(__APPLE__)
  std::string candidate = base + ".dylib";
#else
  std::string candidate = base + ".so";
#endif

  if (PathExists(candidate))
    return candidate;
  return "";
}

static void Backend_Navigate(void* data, uint32_t window_id, const char* url) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && url) {
    // The window's top-level frame may load this URL's origin from now on
    // (laufey_navigation_guard.h); recorded before the load starts, so the
    // load's own check passes.
    laufey_common::NavigationGuard::Instance().SetWindowUrl(window_id, url);
    // The embedder's own load is not reported to will-navigate (API 56).
    laufey_common::WillNavigateRequests::Instance().ExpectEmbedderNavigation(
        window_id, url);
    backend->Navigate(window_id, url);
  }
}

static void Backend_SetTitle(void* data, uint32_t window_id,
                             const char* title) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && title) {
    backend->SetTitle(window_id, title);
  }
}

static void Backend_ExecuteJs(void* data, uint32_t window_id,
                              const char* script, laufey_js_result_fn callback,
                              void* callback_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && script) {
    backend->ExecuteJs(window_id, script, callback, callback_data);
  }
}

static void Backend_Quit(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->Quit();
  }
}

// Microphone and camera requests (API 53; laufey_media_capture.h).
static void Backend_SetMediaCaptureHandler(void* /*data*/,
                                           laufey_media_capture_fn handler,
                                           void* user_data) {
  laufey_common::MediaCaptureRequests::Instance().SetHandler(handler,
                                                             user_data);
}
static bool Backend_AnswerMediaCapture(void* /*data*/, uint64_t request_id,
                                       bool allow) {
  return laufey_common::MediaCaptureRequests::Instance().Answer(request_id,
                                                                allow);
}

// Will-navigate (API 56; laufey_will_navigate.h).
static void Backend_SetWillNavigateHandler(void* /*data*/,
                                           laufey_will_navigate_fn handler,
                                           void* user_data) {
  laufey_common::WillNavigateRequests::Instance().SetHandler(handler,
                                                             user_data);
}
static bool Backend_AnswerWillNavigate(void* /*data*/, uint64_t request_id,
                                       uint32_t decision) {
  return laufey_common::WillNavigateRequests::Instance().Answer(request_id,
                                                                decision);
}

// Quit requests (API 52; laufey_quit_request.h).
static void Backend_SetQuitRequestedHandler(void* data,
                                            laufey_quit_requested_fn handler,
                                            void* user_data) {
  laufey_common::SetQuitRequestedHandler(handler, user_data);
  LaufeyBackend* backend = static_cast<RuntimeLoader*>(data)->GetBackend();
  if (backend && handler)
    backend->WatchSessionEnd();
}
static void Backend_AnswerQuitRequest(void* /*data*/, bool allow) {
  laufey_common::AnswerQuitRequest(allow);
}

static bool Backend_QuitRequestPending(void* /*data*/) {
  return laufey_common::QuitRequestPending();
}

// exit_app (API 46): quit() with an exit code. The host returns it once its
// loop has ended and the web views are released (on Windows through
// laufey_common::EndProcess), and Shutdown doesn't wait for a runtime thread
// that may be blocked for good in its exit().
static void Backend_ExitApp(void* data, int exit_code) {
  laufey_common::MarkExitRequested(exit_code);
  Backend_Quit(data);
}

static void Backend_SetWindowSize(void* data, uint32_t window_id, int width,
                                  int height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetWindowSize(window_id, width, height);
  }
}

static void Backend_GetWindowSize(void* data, uint32_t window_id, int* width,
                                  int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->GetWindowSize(window_id, width, height);
  }
}

static void Backend_GetWindowOuterSize(void* data, uint32_t window_id,
                                       int* width, int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->GetWindowOuterSize(window_id, width, height);
  }
}

static void Backend_SetWindowPosition(void* data, uint32_t window_id, int x,
                                      int y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetWindowPosition(window_id, x, y);
  }
}

static void Backend_GetWindowPosition(void* data, uint32_t window_id, int* x,
                                      int* y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->GetWindowPosition(window_id, x, y);
  }
}

static void Backend_GetWindowInnerPosition(void* data, uint32_t window_id,
                                           int* x, int* y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->GetWindowInnerPosition(window_id, x, y);
  }
}

static void Backend_SetResizable(void* data, uint32_t window_id,
                                 bool resizable) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetResizable(window_id, resizable);
  }
}

static bool Backend_IsResizable(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->IsResizable(window_id);
  }
  return false;
}

static void Backend_SetAlwaysOnTop(void* data, uint32_t window_id,
                                   bool always_on_top) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetAlwaysOnTop(window_id, always_on_top);
  }
}

static bool Backend_IsAlwaysOnTop(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->IsAlwaysOnTop(window_id);
  }
  return false;
}

static void Backend_SetWindowOpacity(void* data, uint32_t window_id,
                                     double opacity) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetWindowOpacity(window_id, opacity);
  }
}

static double Backend_GetWindowOpacity(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->GetWindowOpacity(window_id);
  }
  return 1.0;
}

static double Backend_GetWindowScaleFactor(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->GetWindowScaleFactor(window_id);
  }
  return 1.0;
}

static void Backend_SetClickPassthrough(void* data, uint32_t window_id,
                                        bool enabled) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetClickPassthrough(window_id, enabled);
  }
}

static bool Backend_IsClickPassthrough(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->IsClickPassthrough(window_id);
  }
  return false;
}

static void Backend_SetClickPassthroughForward(void* data, uint32_t window_id,
                                               bool forward) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->SetClickPassthroughForward(window_id, forward);
  }
}

static bool Backend_IsClickPassthroughForward(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->IsClickPassthroughForward(window_id);
  }
  return false;
}

// --- Overlay windows and tray events (API 50) -------------------------------

static LaufeyBackend* OverlayBackendOf(void* data) {
  return static_cast<RuntimeLoader*>(data)->GetBackend();
}

static uint32_t Backend_OverlayCapabilities(void* data) {
  LaufeyBackend* backend = OverlayBackendOf(data);
  return backend ? backend->OverlayCapabilities() : 0;
}

static void Backend_SetInputRegion(void* data, uint32_t window_id,
                                   const laufey_rect_t* rects, size_t count) {
  if (LaufeyBackend* backend = OverlayBackendOf(data))
    backend->SetInputRegion(window_id,
                            laufey_common::MakeInputRegion(rects, count));
}

static bool Backend_SetSkipTaskbar(void* data, uint32_t window_id, bool skip) {
  LaufeyBackend* backend = OverlayBackendOf(data);
  return backend && backend->SetSkipTaskbar(window_id, skip);
}

static bool Backend_SetVisibleOnAllWorkspaces(void* data, uint32_t window_id,
                                              bool visible, bool full_screen) {
  LaufeyBackend* backend = OverlayBackendOf(data);
  return backend &&
         backend->SetVisibleOnAllWorkspaces(window_id, visible, full_screen);
}

static bool Backend_SetWindowLevel(void* data, uint32_t window_id,
                                   int32_t level) {
  LaufeyBackend* backend = OverlayBackendOf(data);
  return backend && laufey_common::IsValidWindowLevel(level) &&
         backend->SetWindowLevel(window_id, level);
}

// --- Background throttling (API 51) -----------------------------------------

static int32_t Backend_GetBackgroundThrottling(void* data, uint32_t window_id) {
  LaufeyBackend* backend = OverlayBackendOf(data);
  return backend ? backend->GetBackgroundThrottling(window_id) : -1;
}

static void Backend_SetTrayEventHandler(void* data, uint32_t tray_id,
                                        laufey_tray_event_fn handler,
                                        void* user_data) {
  if (LaufeyBackend* backend = OverlayBackendOf(data))
    backend->SetTrayEventHandler(tray_id, handler, user_data);
}

static uint32_t Backend_TrayCapabilities(void* data) {
  LaufeyBackend* backend = OverlayBackendOf(data);
  return backend ? backend->TrayCapabilities() : 0;
}

static bool Backend_PopUpTrayMenu(void* data, uint32_t tray_id) {
  LaufeyBackend* backend = OverlayBackendOf(data);
  return backend && backend->PopUpTrayMenu(tray_id);
}

static bool Backend_IsVisible(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    return backend->IsVisible(window_id);
  }
  return false;
}

static void Backend_Show(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->Show(window_id);
  }
}

static void Backend_Hide(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->Hide(window_id);
  }
}

static void Backend_Focus(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->Focus(window_id);
  }
}

static void Backend_PostUiTask(void* data, void (*task)(void*),
                               void* task_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && task) {
    backend->PostUiTask(task, task_data);
  }
}

static void Backend_SetJsCallHandler(void* data, laufey_js_call_fn handler,
                                     void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetJsCallHandler(handler, user_data);
}

static void Backend_SetJsCallHandlerEx(void* data, laufey_js_call_ex_fn handler,
                                       void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetJsCallHandlerEx(handler, user_data);
}

static void Backend_JsCallRespond(void* data, uint64_t call_id,
                                  laufey_value_t* result,
                                  laufey_value_t* error) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  // An id this backend never issued, or a second answer, reaches nothing.
  laufey_common::JsCallRoute route;
  if (!loader->TakeJsCall(call_id, &route))
    return;
  laufey::ValuePtr resultPtr =
      (result && result->value) ? result->value : laufey::Value::Null();
  // Keep the absent-error case as a genuine null pointer. RespondToJsCall on
  // macOS/Windows decides resolve-vs-reject by the pointer's truthiness, so
  // fabricating a Value::Null() here would make every response look like a
  // rejection and resolve the JS promise with null.
  laufey::ValuePtr errorPtr = (error && error->value) ? error->value : nullptr;
  loader->JsCallRespond(route.window_id, route.page_call_id, resultPtr,
                        errorPtr);
}

// --- Custom URL scheme handling ---

static void Backend_RegisterSchemeHandler(void* data, const char* scheme,
                                          laufey_scheme_request_fn handler,
                                          laufey_scheme_cancel_fn on_cancel,
                                          void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetSchemeRequestHandler(scheme ? scheme : "", handler, on_cancel,
                                  user_data);
}

static intptr_t Backend_SchemeRequestReadBody(
    void* /*data*/, laufey_scheme_exchange_t* exchange, uint8_t* buf,
    size_t cap) {
  return reinterpret_cast<SchemeExchangeBase*>(exchange)->ReadRequestBody(buf,
                                                                          cap);
}

// The frame a scheme request came from (API 56), as the backend recorded
// it on the exchange.
static uint32_t Backend_SchemeRequestFrame(void* /*data*/,
                                           laufey_scheme_exchange_t* exchange) {
  if (!exchange)
    return LAUFEY_FRAME_UNKNOWN;
  return reinterpret_cast<SchemeExchangeBase*>(exchange)->frame_kind;
}

static void Backend_SchemeResponseBegin(void* /*data*/,
                                        laufey_scheme_exchange_t* exchange,
                                        int status, const char* headers,
                                        size_t headers_len) {
  reinterpret_cast<SchemeExchangeBase*>(exchange)->Begin(status, headers,
                                                         headers_len);
}

static intptr_t Backend_SchemeResponseWrite(void* /*data*/,
                                            laufey_scheme_exchange_t* exchange,
                                            const uint8_t* buf, size_t len) {
  return reinterpret_cast<SchemeExchangeBase*>(exchange)->WriteResponse(buf,
                                                                        len);
}

static void Backend_SchemeResponseFinish(void* /*data*/,
                                         laufey_scheme_exchange_t* exchange) {
  reinterpret_cast<SchemeExchangeBase*>(exchange)->Finish();
}

static void Backend_InvokeJsCallback(void* data, uint64_t callback_id,
                                     laufey_value_t* args) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    laufey::ValuePtr argsPtr =
        (args && args->value) ? args->value : laufey::Value::List();
    // Broadcast to window 0 (all windows) since callback_id isn't tied to a
    // window
    backend->InvokeJsCallback(0, callback_id, argsPtr);
  }
}

static void Backend_SetKeyboardEventHandler(void* data,
                                            laufey_keyboard_event_fn handler,
                                            void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetKeyboardEventHandler(handler, user_data);
}

static void Backend_SetMouseClickHandler(void* data,
                                         laufey_mouse_click_fn handler,
                                         void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMouseClickHandler(handler, user_data);
}

static void Backend_SetMouseMoveHandler(void* data,
                                        laufey_mouse_move_fn handler,
                                        void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMouseMoveHandler(handler, user_data);
}

static void Backend_SetWheelHandler(void* data, laufey_wheel_fn handler,
                                    void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetWheelHandler(handler, user_data);
}

static void Backend_SetCursorEnterLeaveHandler(
    void* data, laufey_cursor_enter_leave_fn handler, void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetCursorEnterLeaveHandler(handler, user_data);
}

static void Backend_SetFocusedHandler(void* data, laufey_focused_fn handler,
                                      void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetFocusedHandler(handler, user_data);
}

static void Backend_SetResizeHandler(void* data, laufey_resize_fn handler,
                                     void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetResizeHandler(handler, user_data);
}

static void Backend_SetMoveHandler(void* data, laufey_move_fn handler,
                                   void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMoveHandler(handler, user_data);
}

static void Backend_ReleaseJsCallback(void* data, uint64_t callback_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->ReleaseJsCallback(0, callback_id);
  }
}

static void Backend_PollJsCalls(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->PollPendingJsCalls();
}

static void Backend_SetJsCallNotify(void* data, void (*notify_fn)(void*),
                                    void* notify_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetJsCallNotify(notify_fn, notify_data);
}

static void Backend_SetApplicationMenu(void* data, uint32_t window_id,
                                       laufey_value_t* menu_template,
                                       laufey_menu_click_fn on_click,
                                       void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && menu_template) {
    // Clicks also reach the app-level handler (API 55).
    laufey_common::AppMenuClickFn click =
        laufey_common::WrapAppMenuClick(on_click, on_click_data);
    backend->SetApplicationMenu(window_id, menu_template,
                                &loader->GetBackendApi(), click.fn, click.data);
  }
}

// The app's own application menu (API 56): macOS only (the backend's
// SetAppMenu answers false elsewhere).
static bool Backend_SetAppMenu(void* data, laufey_value_t* menu_template,
                               laufey_menu_click_fn on_click,
                               void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  return backend && backend->SetAppMenu(menu_template, &loader->GetBackendApi(),
                                        on_click, on_click_data);
}

// Application-menu clicks for the app (API 55): every backend builds its
// application menus with laufey_common::WrapAppMenuClick.
static void Backend_SetAppMenuClickHandler(void* /*data*/,
                                           laufey_menu_click_fn handler,
                                           void* user_data) {
  laufey_common::SetAppMenuClickHandler(handler, user_data);
}

static void Backend_ShowContextMenu(void* data, uint32_t window_id, int x,
                                    int y, laufey_value_t* menu_template,
                                    laufey_menu_click_fn on_click,
                                    void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend && menu_template) {
    backend->ShowContextMenu(window_id, x, y, menu_template,
                             &loader->GetBackendApi(), on_click, on_click_data);
  }
}

static void Backend_OpenDevTools(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->OpenDevTools(window_id);
  }
}

static void Backend_PrintToPdf(void* data, uint32_t window_id,
                               laufey_pdf_result_fn callback,
                               void* callback_data) {
  if (!callback)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->PrintToPdf(window_id, callback, callback_data);
  } else {
    callback(nullptr, 0, "backend not initialized", callback_data);
  }
}

static void Backend_SetJsNamespace(void* data, const char* name) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (name) {
    loader->SetJsNamespace(name);
  }
}

static uint32_t Backend_CreateWindow(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  uint32_t window_id = loader->AllocateWindowId();
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->CreateWindow(window_id, 800, 600);
  }
  return window_id;
}

static uint32_t Backend_CreateWindowEx(void* data, uint32_t flags) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  uint32_t window_id = loader->AllocateWindowId();
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->CreateWindowEx(window_id, 800, 600, flags);
  }
  return window_id;
}

static bool Backend_GetTrayIconBounds(void* data, uint32_t tray_id, int* x,
                                      int* y, int* width, int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend) {
    return false;
  }
  return backend->GetTrayIconBounds(tray_id, x, y, width, height);
}

static void Backend_CloseWindow(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (backend) {
    backend->CloseWindow(window_id);
  }
  laufey_common::NavigationGuard::Instance().ForgetWindow(window_id);
  laufey_common::DownloadPolicy::Instance().ForgetWindow(window_id);
  // A microphone / camera request the window still waits on is refused.
  laufey_common::MediaCaptureRequests::Instance().ForgetWindow(window_id);
  laufey_common::WillNavigateRequests::Instance().ForgetWindow(window_id);
}

static void Backend_SetCloseRequestedHandler(void* data,
                                             laufey_close_requested_fn handler,
                                             void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetCloseRequestedHandler(handler, user_data);
}

// Test hook (API >= 31): synthesize a close-requested event through the same
// dispatch code a real OS close click runs. Returns true if a registered
// handler deferred the close; false means the close proceeded. Proceeds
// through Backend_CloseWindow — the real close entry point — rather than
// re-inlining its body, so the hook can't silently diverge from the
// shipping close path.
static bool Backend_TestTriggerCloseRequested(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  bool proceed = loader->DispatchCloseRequestedEvent(window_id);
  if (proceed) {
    Backend_CloseWindow(data, window_id);
    return false;
  }
  return true;
}

static void Backend_SetPageLoadHandler(void* data, laufey_page_load_fn handler,
                                       void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetPageLoadHandler(handler, user_data);
}

static int Backend_ShowDialog(void* data, uint32_t window_id, int dialog_type,
                              const char* title, const char* message,
                              const char* default_value,
                              char** out_input_value) {
  if (out_input_value)
    *out_input_value = nullptr;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend)
    return 0;
  std::string t = title ? title : "";
  std::string m = message ? message : "";
  std::string d = default_value ? default_value : "";
  return backend->ShowDialog(window_id, dialog_type, t, m, d, out_input_value);
}

static void Backend_StringFree(void* /*backend_data*/, char* s) {
  if (s)
    free(s);
}

static char* Backend_ReadClipboardText(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->ReadClipboardText();
  return nullptr;
}

static void Backend_WriteClipboardText(void* data, const char* text) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->WriteClipboardText(text ? text : "");
}

// --- Dock / taskbar ---

static void Backend_SetDockBadge(void* data, const char* badge_or_null) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetDockBadge(badge_or_null);
  }
}

static void Backend_BounceDock(void* data, int type) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->BounceDock(type);
  }
}

static void Backend_SetDockMenu(void* data, laufey_value_t* menu_template,
                                laufey_menu_click_fn on_click,
                                void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetDockMenu(menu_template, &loader->GetBackendApi(), on_click,
                         on_click_data);
  }
}

static void Backend_SetDockVisible(void* data, bool visible) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetDockVisible(visible);
  }
}

static void Backend_SetDockReopenHandler(void* data,
                                         laufey_dock_reopen_fn handler,
                                         void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetDockReopenHandler(handler, user_data);
  }
}

// --- Deep links / custom URL schemes (macOS only) ---
#if defined(__APPLE__)

static void Backend_SetOpenUrlHandler(void* data, laufey_open_url_fn handler,
                                      void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetOpenUrlHandler(handler, user_data);
  }
}

static bool Backend_TestTriggerOpenUrl(void* data, const char* url) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    return backend->TestTriggerOpenUrl(url);
  }
  return false;
}

#endif  // defined(__APPLE__)

// --- Single instance ---

static void Backend_SetSecondInstanceHandler(void* data,
                                             laufey_second_instance_fn handler,
                                             void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->SetSecondInstanceHandler(handler, user_data);
  }
}

// --- Passkeys ---

static uint32_t Backend_PasskeyCapabilities(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->PasskeyCapabilities();
  return 0;
}

static void Backend_Share(void* data, uint32_t window_id,
                          const char* request_json, laufey_share_fn callback,
                          void* user_data) {
  laufey_common::ShareRequest request;
  std::string error;
  if (!laufey_common::ParseShareRequest(request_json ? request_json : "",
                                        &request, &error)) {
    laufey_common::ShareReport(callback, user_data, LAUFEY_SHARE_FAILED, error);
    return;
  }
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->Share(window_id, request, callback, user_data);
  } else {
    laufey_common::ShareReport(callback, user_data, LAUFEY_SHARE_FAILED,
                               "backend not initialized");
  }
}

static void Backend_PasskeyRequest(void* data, uint32_t window_id,
                                   uint32_t kind, const char* options_json,
                                   laufey_passkey_result_fn callback,
                                   void* user_data) {
  if (!callback)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->PasskeyRequest(window_id, kind, options_json, callback, user_data);
  } else {
    callback(user_data,
             "{\"ok\":false,\"error\":{\"code\":\"unknown\","
             "\"message\":\"backend not initialized\"}}");
  }
}

// --- Drag and drop, file dialogs, rich clipboard (API >= 39) ---

static LaufeyBackend* BackendOf(void* data) {
  return static_cast<RuntimeLoader*>(data)->GetBackend();
}

static void Backend_SetFileDropHandler(void* data, laufey_file_drop_fn handler,
                                       void* user_data) {
  if (LaufeyBackend* backend = BackendOf(data))
    backend->SetFileDropHandler(handler, user_data);
}

static bool Backend_TestTriggerFileDrop(void* data, uint32_t window_id,
                                        int phase, double x, double y,
                                        const char* const* paths,
                                        size_t count) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->TestTriggerFileDrop(window_id, phase, x, y, paths, count);
  return false;
}

static void Backend_StartFileDrag(void* data, uint32_t window_id,
                                  const char* const* paths, size_t count,
                                  const uint8_t* icon_png, size_t icon_len,
                                  laufey_drag_result_fn callback,
                                  void* user_data) {
  if (LaufeyBackend* backend = BackendOf(data)) {
    backend->StartFileDrag(window_id, paths, count, icon_png, icon_len,
                           callback, user_data);
  } else if (callback) {
    callback(user_data, LAUFEY_DRAG_RESULT_FAILED);
  }
}

static uint32_t Backend_ShowFileDialog(
    void* data, uint32_t window_id, const laufey_file_dialog_options_t* options,
    laufey_file_dialog_result_fn callback, void* user_data) {
  if (!callback)
    return 0;
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->ShowFileDialog(window_id, options, callback, user_data);
  callback(user_data, 0, LAUFEY_FILE_DIALOG_FAILED, nullptr, 0);
  return 0;
}

static uint32_t Backend_ShowMessageDialog(
    void* data, uint32_t window_id,
    const laufey_message_dialog_options_t* options,
    laufey_message_dialog_result_fn callback, void* user_data) {
  if (!callback)
    return 0;
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->ShowMessageDialog(window_id, options, callback, user_data);
  callback(user_data, 0, LAUFEY_MESSAGE_DIALOG_FAILED, -1);
  return 0;
}

static bool Backend_CancelFileDialog(void* data, uint32_t dialog_id) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->CancelFileDialog(dialog_id);
  return false;
}

static bool Backend_TestFileDialogRespond(void* data, int action,
                                          const char* path) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->TestFileDialogRespond(action, path);
  return false;
}

static uint32_t Backend_ClipboardCapabilities(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->ClipboardCapabilities();
  return 0;
}

static char* Backend_ReadClipboardHtml(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->ReadClipboardHtml();
  return nullptr;
}

static bool Backend_WriteClipboardHtml(void* data, const char* html,
                                       const char* text_or_null) {
  if (!html)
    return false;
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->WriteClipboardHtml(html, text_or_null);
  return false;
}

static uint8_t* Backend_ReadClipboardImage(void* data, size_t* len_out) {
  if (len_out)
    *len_out = 0;
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->ReadClipboardImage(len_out);
  return nullptr;
}

static bool Backend_WriteClipboardImage(void* data, const uint8_t* png,
                                        size_t len) {
  if (!png || len == 0)
    return false;
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->WriteClipboardImage(png, len);
  return false;
}

static char* Backend_ReadClipboardFormats(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->ReadClipboardFormats();
  return nullptr;
}

static void Backend_SetClipboardChangeHandler(void* data,
                                              laufey_clipboard_change_fn fn,
                                              void* user_data) {
  if (LaufeyBackend* backend = BackendOf(data))
    backend->SetClipboardChangeHandler(fn, user_data);
}

static void Backend_BufferFree(void* /*data*/, void* buffer) {
  free(buffer);
}

// --- Global shortcuts, launch at login, DevTools (API >= 40) ---

static uint32_t Backend_SystemCapabilities(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->SystemCapabilities();
  return 0;
}

static void Backend_SetShortcutHandler(void* data, laufey_shortcut_fn handler,
                                       void* user_data) {
  if (LaufeyBackend* backend = BackendOf(data))
    backend->SetShortcutHandler(handler, user_data);
}

static void Backend_RegisterShortcut(void* data, const char* accelerator,
                                     laufey_shortcut_result_fn callback,
                                     void* user_data) {
  if (LaufeyBackend* backend = BackendOf(data)) {
    backend->RegisterShortcut(accelerator, callback, user_data);
  } else if (callback) {
    callback(user_data, LAUFEY_SHORTCUT_NOT_SUPPORTED, nullptr);
  }
}

static bool Backend_UnregisterShortcut(void* data, const char* accelerator) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->UnregisterShortcut(accelerator);
  return false;
}

static void Backend_UnregisterAllShortcuts(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    backend->UnregisterAllShortcuts();
}

static char* Backend_ListShortcuts(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->ListShortcuts();
  return nullptr;
}

static char* Backend_PlatformFeatures(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->PlatformFeatures();
  return nullptr;
}

static char* Backend_TrayUnavailableReason(void* /*data*/) {
  return laufey_common::TrayUnavailableReasonForAbi();
}

// On Linux it fires on the GTK main thread (it runs the default GLib main
// context, where the probe's NameOwnerChanged subscription delivers).
static void Backend_SetPlatformFeaturesChangedHandler(
    void* /*data*/, laufey_platform_features_changed_fn handler,
    void* user_data) {
  laufey_common::SetPlatformFeaturesChangedHandler(handler, user_data);
}

// Title bar preferences (API 47). The change handler fires on the
// watcher's own thread (Linux, Windows) or the main thread (macOS).
static char* Backend_TitleBarPreferences(void* /*data*/) {
  return laufey_common::TitleBarPreferencesJsonForAbi();
}

static void Backend_SetTitleBarPreferencesChangedHandler(
    void* /*data*/, laufey_title_bar_preferences_changed_fn handler,
    void* user_data) {
  laufey_common::SetTitleBarPreferencesChangedHandler(handler, user_data);
}

static char* Backend_CanonicalizeAccelerator(void* data,
                                             const char* accelerator) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->CanonicalizeAccelerator(accelerator);
  return nullptr;
}

static bool Backend_TestTriggerShortcut(void* data, const char* accelerator) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->TestTriggerShortcut(accelerator);
  return false;
}

static int Backend_GetLaunchAtLogin(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->GetLaunchAtLogin();
  return LAUFEY_LOGIN_ITEM_NOT_SUPPORTED;
}

static int Backend_SetLaunchAtLogin(void* data, bool enabled,
                                    char** error_out) {
  if (error_out)
    *error_out = nullptr;
  LaufeyBackend* backend = BackendOf(data);
  if (!backend)
    return LAUFEY_LOGIN_ITEM_NOT_SUPPORTED;
  std::string error;
  int state = backend->SetLaunchAtLogin(enabled, &error);
  if (state == LAUFEY_LOGIN_ITEM_FAILED && error_out && !error.empty()) {
    char* copy = static_cast<char*>(malloc(error.size() + 1));
    if (copy) {
      memcpy(copy, error.c_str(), error.size() + 1);
      *error_out = copy;
    }
  }
  return state;
}

static void Backend_CloseDevTools(void* data, uint32_t window_id) {
  if (LaufeyBackend* backend = BackendOf(data))
    backend->CloseDevTools(window_id);
}

static bool Backend_IsDevToolsOpen(void* data, uint32_t window_id) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->IsDevToolsOpen(window_id);
  return false;
}

static bool Backend_IsDevToolsEnabled(void* data, uint32_t window_id) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->IsDevToolsEnabled(window_id);
  return false;
}

// --- Window state, constraints, screens and chrome (API >= 38) ---

static uint32_t Backend_WindowCapabilities(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->WindowCapabilities();
  return 0;
}

static void Backend_SetWindowState(void* data, uint32_t window_id, int action) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetWindowState(window_id, action);
}

static uint32_t Backend_GetWindowState(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->GetWindowState(window_id);
  return 0;
}

static void Backend_SetWindowStateHandler(void* data,
                                          laufey_window_state_fn handler,
                                          void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetWindowStateHandler(handler, user_data);
}

static void Backend_SetWindowSizeConstraints(void* data, uint32_t window_id,
                                             int min_width, int min_height,
                                             int max_width, int max_height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetWindowSizeConstraints(window_id, min_width, min_height,
                                      max_width, max_height);
}

static void Backend_GetWindowSizeConstraints(void* data, uint32_t window_id,
                                             int* min_width, int* min_height,
                                             int* max_width, int* max_height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->GetWindowSizeConstraints(window_id, min_width, min_height,
                                      max_width, max_height);
    return;
  }
  for (int* p : {min_width, min_height, max_width, max_height}) {
    if (p)
      *p = 0;
  }
}

static size_t Backend_GetScreens(void* data, laufey_screen_t* out,
                                 size_t capacity) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->GetScreens(out, capacity);
  return 0;
}

static int64_t Backend_GetWindowScreen(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->GetWindowScreen(window_id);
  return 0;
}

static void Backend_SetDisplayChangedHandler(void* data,
                                             laufey_display_changed_fn handler,
                                             void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetDisplayChangedHandler(handler, user_data);
}

static bool Backend_SetWindowTitlebarStyle(void* data, uint32_t window_id,
                                           int style) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->SetWindowTitlebarStyle(window_id, style);
  return false;
}

static bool Backend_SetWindowTitleVisibility(void* data, uint32_t window_id,
                                             int visibility) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->SetWindowTitleVisibility(window_id, visibility);
  return false;
}

static bool Backend_SetWindowToolbarStyle(void* data, uint32_t window_id,
                                          int style) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->SetWindowToolbarStyle(window_id, style);
  return false;
}

// --- Child web views (API 58) ---

static uint32_t Backend_WebViewCapabilities(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->WebViewCapabilities();
  return 0;
}

static uint32_t Backend_CreateWebView(void* data, uint32_t window_id,
                                      const char* options_json,
                                      laufey_webview_event_fn on_event,
                                      void* user_data, int32_t* status_out) {
  int32_t ignored = 0;
  if (!status_out)
    status_out = &ignored;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend) {
    *status_out = LAUFEY_WEBVIEW_UNSUPPORTED;
    return 0;
  }
  if (!options_json || !on_event) {
    *status_out = LAUFEY_WEBVIEW_INVALID;
    return 0;
  }
  *status_out = LAUFEY_WEBVIEW_OK;
  return backend->CreateWebView(window_id, options_json, on_event, user_data,
                                status_out);
}

static bool Backend_WebViewSetBounds(void* data, uint32_t webview_id, double x,
                                     double y, double width, double height) {
  if (!laufey_common::IsValidWebViewBounds(x, y, width, height))
    return false;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->WebViewSetBounds(webview_id, x, y, width, height);
  return false;
}

static bool Backend_WebViewSetVisible(void* data, uint32_t webview_id,
                                      bool visible) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->WebViewSetVisible(webview_id, visible);
  return false;
}

static int32_t Backend_WebViewNavigate(void* data, uint32_t webview_id,
                                       const char* url) {
  if (!url || !laufey_common::IsAllowedWebViewUrl(url))
    return LAUFEY_WEBVIEW_NOT_ALLOWED_URL;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->WebViewNavigate(webview_id, url);
  return LAUFEY_WEBVIEW_UNSUPPORTED;
}

// Applies the registry's order to the view's window (z-order changes).
static bool RestackWindowOf(LaufeyBackend* backend, uint32_t webview_id) {
  auto& sinks = laufey_common::WebViewSinks::Instance();
  uint32_t window_id = sinks.WindowOf(webview_id);
  return window_id != 0 &&
         backend->WebViewRestack(window_id, sinks.StackOrder(window_id));
}

static bool Backend_WebViewCommand(void* data, uint32_t webview_id,
                                   int32_t command) {
  if (command < LAUFEY_WEBVIEW_COMMAND_BACK ||
      command > LAUFEY_WEBVIEW_COMMAND_LOWER)
    return false;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend)
    return false;
  if (command == LAUFEY_WEBVIEW_COMMAND_RAISE ||
      command == LAUFEY_WEBVIEW_COMMAND_LOWER) {
    if (!(backend->WebViewCapabilities() & LAUFEY_WEBVIEW_CAP_Z_ORDER))
      return false;
    auto& sinks = laufey_common::WebViewSinks::Instance();
    bool known = command == LAUFEY_WEBVIEW_COMMAND_RAISE
                     ? sinks.Raise(webview_id)
                     : sinks.Lower(webview_id);
    return known && RestackWindowOf(backend, webview_id);
  }
  return backend->WebViewCommand(webview_id, command);
}

static bool Backend_WebViewSetZoom(void* data, uint32_t webview_id,
                                   double factor) {
  if (!laufey_common::IsValidWebViewZoom(factor))
    return false;
  LaufeyBackend* backend = static_cast<RuntimeLoader*>(data)->GetBackend();
  return backend && backend->WebViewSetZoom(webview_id, factor);
}

static double Backend_WebViewGetZoom(void* data, uint32_t webview_id) {
  LaufeyBackend* backend = static_cast<RuntimeLoader*>(data)->GetBackend();
  return backend ? backend->WebViewGetZoom(webview_id) : 0;
}

static bool Backend_WebViewFind(void* data, uint32_t webview_id,
                                const char* text, uint32_t flags) {
  if (!text || !*text)
    return false;
  LaufeyBackend* backend = static_cast<RuntimeLoader*>(data)->GetBackend();
  return backend &&
         backend->WebViewFind(webview_id, text,
                              flags & (LAUFEY_WEBVIEW_FIND_FORWARD |
                                       LAUFEY_WEBVIEW_FIND_MATCH_CASE));
}

static bool Backend_WebViewStopFind(void* data, uint32_t webview_id) {
  LaufeyBackend* backend = static_cast<RuntimeLoader*>(data)->GetBackend();
  return backend && backend->WebViewStopFind(webview_id);
}

static int32_t Backend_WebViewOpenDevTools(void* data, uint32_t webview_id) {
  LaufeyBackend* backend = static_cast<RuntimeLoader*>(data)->GetBackend();
  return backend ? backend->WebViewOpenDevTools(webview_id)
                 : LAUFEY_WEBVIEW_UNSUPPORTED;
}

static bool Backend_WebViewPrint(void* data, uint32_t webview_id) {
  LaufeyBackend* backend = static_cast<RuntimeLoader*>(data)->GetBackend();
  return backend && backend->WebViewPrint(webview_id);
}

static bool Backend_WebViewSetZOrder(void* data, uint32_t webview_id,
                                     int32_t z_order) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend ||
      !(backend->WebViewCapabilities() & LAUFEY_WEBVIEW_CAP_Z_ORDER))
    return false;
  return laufey_common::WebViewSinks::Instance().SetZOrder(webview_id,
                                                           z_order) &&
         RestackWindowOf(backend, webview_id);
}

static char* Backend_WebViewState(void* data, uint32_t webview_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend)
    return nullptr;
  std::string state = backend->WebViewState(webview_id);
  if (state.empty())
    return nullptr;
  char* copy = static_cast<char*>(malloc(state.size() + 1));
  if (copy)
    memcpy(copy, state.c_str(), state.size() + 1);
  return copy;
}

static void Backend_WebViewEvaluate(void* data, uint32_t webview_id,
                                    const char* script,
                                    laufey_webview_eval_fn callback,
                                    void* user_data) {
  if (!callback)
    return;
  if (!script) {
    callback(user_data, LAUFEY_WEBVIEW_INVALID, "no script");
    return;
  }
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend) {
    callback(user_data, LAUFEY_WEBVIEW_UNSUPPORTED,
             "this backend has no web views");
    return;
  }
  backend->WebViewEvaluate(webview_id, script, callback, user_data);
}

static void Backend_WebViewCapture(void* data, uint32_t webview_id,
                                   int32_t format, int32_t quality,
                                   double scale,
                                   laufey_webview_capture_fn callback,
                                   void* user_data) {
  if (!callback)
    return;
  if (format != LAUFEY_WEBVIEW_CAPTURE_PNG &&
      format != LAUFEY_WEBVIEW_CAPTURE_JPEG) {
    callback(user_data, LAUFEY_WEBVIEW_INVALID, nullptr, 0,
             "unknown image format");
    return;
  }
  quality = quality < 0 ? 0 : (quality > 100 ? 100 : quality);
  // Non-finite or <= 0: device pixels; at most 4 output pixels per CSS px.
  if (!(scale > 0))
    scale = 0;
  if (scale > 4)
    scale = 4;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend) {
    callback(user_data, LAUFEY_WEBVIEW_UNSUPPORTED, nullptr, 0,
             "this backend has no web views");
    return;
  }
  backend->WebViewCapture(webview_id, format, quality, scale, callback,
                          user_data);
}

static bool Backend_WebViewClose(void* data, uint32_t webview_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->WebViewClose(webview_id);
  return false;
}

static bool Backend_SetWindowTrafficLightPosition(void* data,
                                                  uint32_t window_id, int x,
                                                  int y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->SetWindowTrafficLightPosition(window_id, x, y);
  return false;
}

static bool Backend_SetWindowBackdrop(void* data, uint32_t window_id,
                                      int backdrop, int material) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->SetWindowBackdrop(window_id, backdrop, material);
  return false;
}

static bool Backend_GetWindowNormalBounds(void* data, uint32_t window_id,
                                          int* x, int* y, int* width,
                                          int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->GetWindowNormalBounds(window_id, x, y, width, height);
  return false;
}

static void Backend_SetQuitOnLastWindowClosed(void* data, bool quit) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetQuitOnLastWindowClosed(quit);
}

// --- UI-thread tasks (API >= 42) ---

static void Backend_DispatchUiTask(void* /*data*/, laufey_ui_task_fn task,
                                   void* task_data) {
  laufey_common::UiTaskDispatcher::Get().Dispatch(task, task_data);
}

static bool Backend_IsUiThread(void* /*data*/) {
  return laufey_common::UiTaskDispatcher::Get().IsUiThread();
}

// --- Auth session (API >= 42) ---

static uint32_t Backend_AuthSessionCapabilities(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->AuthSessionCapabilities();
  return 0;
}

static void Backend_AuthSessionStart(void* data, uint32_t window_id,
                                     const char* url, const char* callback,
                                     uint32_t flags,
                                     laufey_auth_session_result_fn on_result,
                                     void* user_data) {
  if (!on_result)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->AuthSessionStart(window_id, url, callback, flags, on_result,
                              user_data);
  } else {
    on_result(user_data, LAUFEY_AUTH_SESSION_FAILED, "backend not initialized");
  }
}

static bool Backend_TestCancelAuthSession(void* /*data*/) {
  return laufey_common::AuthSessionCancelCurrent(
      "the user cancelled the sign-in");
}

// API >= 43: the app cancels the running session (no session runs off
// macOS, so it answers false there).
static bool Backend_AuthSessionCancel(void* /*data*/) {
  return laufey_common::AuthSessionCancelCurrent(
      "the app cancelled the sign-in");
}

// --- Tray / status bar ---

static uint32_t Backend_CreateTrayIcon(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    return backend->CreateTrayIcon();
  return 0;
}

static void Backend_DestroyTrayIcon(void* data, uint32_t tray_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->DestroyTrayIcon(tray_id);
}

static void Backend_SetTrayIcon(void* data, uint32_t tray_id,
                                const void* png_bytes, size_t len) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayIcon(tray_id, png_bytes, len);
}

static void Backend_SetTrayTooltip(void* data, uint32_t tray_id,
                                   const char* tooltip_or_null) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayTooltip(tray_id, tooltip_or_null);
}

// Test hook (API >= 30): synthesize a click on a menu/tray item by id. Platform
// independent — the shared registry in backend-common holds the handlers.
static bool Backend_TestClickMenuItem(void* /*data*/, const char* item_id) {
  return laufey_common::TestClickMenuItem(item_id);
}

static void InjectKey(void* ctx, uint32_t window_id, int state, const char* key,
                      const char* code, uint32_t modifiers, bool repeat) {
  static_cast<RuntimeLoader*>(ctx)->DispatchKeyboardEvent(
      window_id, state, key, code, modifiers, repeat);
}
static void InjectClick(void* ctx, uint32_t window_id, int state, int button,
                        double x, double y, uint32_t modifiers,
                        int32_t click_count) {
  static_cast<RuntimeLoader*>(ctx)->DispatchMouseClickEvent(
      window_id, state, button, x, y, modifiers, click_count);
}
static void InjectMove(void* ctx, uint32_t window_id, double x, double y,
                       uint32_t modifiers) {
  static_cast<RuntimeLoader*>(ctx)->DispatchMouseMoveEvent(window_id, x, y,
                                                           modifiers);
}
static void InjectWheel(void* ctx, uint32_t window_id, double delta_x,
                        double delta_y, double x, double y, uint32_t modifiers,
                        int32_t delta_mode) {
  static_cast<RuntimeLoader*>(ctx)->DispatchWheelEvent(
      window_id, delta_x, delta_y, x, y, modifiers, delta_mode);
}
static void InjectEnterLeave(void* ctx, uint32_t window_id, int entered,
                             double x, double y, uint32_t modifiers) {
  static_cast<RuntimeLoader*>(ctx)->DispatchCursorEnterLeaveEvent(
      window_id, entered, x, y, modifiers);
}

static bool Backend_TestInjectInput(void* data, uint32_t window_id,
                                    const laufey_test_input_t* event) {
  laufey_common::TestInjectSink sink = {
      InjectKey, InjectClick, InjectMove, InjectWheel, InjectEnterLeave, data,
  };
  return laufey_common::TestInjectInput(window_id, event, sink);
}

static void Backend_SetTrayMenu(void* data, uint32_t tray_id,
                                laufey_value_t* menu_template,
                                laufey_menu_click_fn on_click,
                                void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayMenu(tray_id, menu_template, &loader->GetBackendApi(),
                         on_click, on_click_data);
}

static void Backend_SetTrayClickHandler(void* data, uint32_t tray_id,
                                        laufey_tray_click_fn handler,
                                        void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayClickHandler(tray_id, handler, user_data);
}

static void Backend_SetTrayDoubleClickHandler(void* data, uint32_t tray_id,
                                              laufey_tray_click_fn handler,
                                              void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayDoubleClickHandler(tray_id, handler, user_data);
}

static void Backend_SetTrayIconDark(void* data, uint32_t tray_id,
                                    const void* png_bytes, size_t len) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->SetTrayIconDark(tray_id, png_bytes, len);
}

static uint32_t Backend_ShowNotification(void* data, laufey_value_t* options,
                                         laufey_notification_event_fn on_event,
                                         void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend) {
    if (options) {
      loader->GetBackendApi().value_free(options);
    }
    return 0;
  }
  return backend->ShowNotification(options, &loader->GetBackendApi(), on_event,
                                   user_data);
}

static void Backend_CloseNotification(void* data, uint32_t notification_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend())
    backend->CloseNotification(notification_id);
}

static void Backend_QueryPermission(void* data, int kind,
                                    laufey_permission_callback_fn cb,
                                    void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->QueryPermission(kind, cb, user_data);
  } else if (cb) {
    cb(user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
  }
}

static void Backend_RequestPermission(void* data, int kind,
                                      laufey_permission_callback_fn cb,
                                      void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (LaufeyBackend* backend = loader->GetBackend()) {
    backend->RequestPermission(kind, cb, user_data);
  } else if (cb) {
    cb(user_data, LAUFEY_PERMISSION_STATUS_UNSUPPORTED);
  }
}

// --- Menus and notifications (API >= 41) ---

static uint32_t Backend_MenuCapabilities(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->MenuCapabilities();
  return 0;
}

static void Backend_ShowContextMenuEx(void* data, uint32_t window_id, int x,
                                      int y, laufey_value_t* menu_template,
                                      laufey_menu_click_fn on_click,
                                      void* on_click_data,
                                      laufey_menu_closed_fn on_closed,
                                      void* on_closed_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  LaufeyBackend* backend = loader->GetBackend();
  if (!backend || !menu_template) {
    if (on_closed)
      on_closed(on_closed_data, window_id);
    return;
  }
  // Every backend parses the template before returning; it is ours to free.
  backend->ShowContextMenuEx(window_id, x, y, menu_template,
                             &loader->GetBackendApi(), on_click, on_click_data,
                             on_closed, on_closed_data);
  loader->GetBackendApi().value_free(menu_template);
}

static bool Backend_TestDismissContextMenu(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->TestDismissContextMenu();
  return false;
}

static bool Backend_TestTriggerMenuAccelerator(void* data, uint32_t window_id,
                                               const char* accelerator) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->TestTriggerMenuAccelerator(window_id, accelerator);
  return false;
}

static uint32_t Backend_NotificationCapabilities(void* data) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->NotificationCapabilities();
  return 0;
}

static void Backend_SetNotificationResponseHandler(
    void* data, laufey_notification_response_fn handler, void* user_data) {
  if (LaufeyBackend* backend = BackendOf(data))
    backend->SetNotificationResponseHandler(handler, user_data);
}

static void Backend_ListScheduledNotifications(void* data,
                                               laufey_notification_list_fn cb,
                                               void* user_data) {
  if (LaufeyBackend* backend = BackendOf(data)) {
    backend->ListScheduledNotifications(cb, user_data);
  } else if (cb) {
    cb(user_data, "[]");
  }
}

static void Backend_CancelNotification(void* data, const char* tag) {
  if (LaufeyBackend* backend = BackendOf(data))
    backend->CancelNotification(tag);
}

static bool Backend_TestNotificationRespond(void* data, const char* tag,
                                            const char* action_id) {
  if (LaufeyBackend* backend = BackendOf(data))
    return backend->TestNotificationRespond(tag, action_id);
  return false;
}

void RuntimeLoader::InitializeBackendApi() {
  memset(&backend_api_, 0, sizeof(backend_api_));
  backend_api_.version = LAUFEY_API_VERSION;
  backend_api_.backend_data = this;
  backend_api_.test_click_menu_item = Backend_TestClickMenuItem;

  backend_api_.navigate = Backend_Navigate;
  backend_api_.set_title = Backend_SetTitle;
  backend_api_.execute_js = Backend_ExecuteJs;
  backend_api_.quit = Backend_Quit;
  backend_api_.set_window_size = Backend_SetWindowSize;
  backend_api_.get_window_size = Backend_GetWindowSize;
  backend_api_.get_window_outer_size = Backend_GetWindowOuterSize;
  backend_api_.set_window_position = Backend_SetWindowPosition;
  backend_api_.get_window_position = Backend_GetWindowPosition;
  backend_api_.get_window_inner_position = Backend_GetWindowInnerPosition;
  backend_api_.set_resizable = Backend_SetResizable;
  backend_api_.is_resizable = Backend_IsResizable;
  backend_api_.set_always_on_top = Backend_SetAlwaysOnTop;
  backend_api_.is_always_on_top = Backend_IsAlwaysOnTop;
  backend_api_.set_window_opacity = Backend_SetWindowOpacity;
  backend_api_.get_window_opacity = Backend_GetWindowOpacity;
  backend_api_.get_window_scale_factor = Backend_GetWindowScaleFactor;
  backend_api_.set_click_passthrough = Backend_SetClickPassthrough;
  backend_api_.is_click_passthrough = Backend_IsClickPassthrough;
  backend_api_.set_click_passthrough_forward =
      Backend_SetClickPassthroughForward;
  backend_api_.is_click_passthrough_forward = Backend_IsClickPassthroughForward;
  // Overlay windows, tray events, the activation policy, speech (API 50).
  backend_api_.overlay_capabilities = Backend_OverlayCapabilities;
  backend_api_.set_input_region = Backend_SetInputRegion;
  backend_api_.set_skip_taskbar = Backend_SetSkipTaskbar;
  backend_api_.set_visible_on_all_workspaces =
      Backend_SetVisibleOnAllWorkspaces;
  backend_api_.set_window_level = Backend_SetWindowLevel;
  backend_api_.set_tray_event_handler = Backend_SetTrayEventHandler;
  backend_api_.tray_capabilities = Backend_TrayCapabilities;
  backend_api_.pop_up_tray_menu = Backend_PopUpTrayMenu;
#if defined(__APPLE__)
  backend_api_.set_activation_policy = [](void*, int32_t policy) -> bool {
    return laufey_common::MacSetActivationPolicy(policy);
  };
  backend_api_.get_activation_policy = [](void*) -> int32_t {
    return laufey_common::MacGetActivationPolicy();
  };
#else
  backend_api_.set_activation_policy = [](void*, int32_t) { return false; };
  backend_api_.get_activation_policy = [](void*) -> int32_t { return -1; };
#endif
  backend_api_.speak = [](void*, const char* json, laufey_speech_fn cb,
                          void* user_data) -> uint32_t {
    return laufey_common::Speak(json, cb, user_data);
  };
  backend_api_.stop_speaking = [](void*) { laufey_common::StopSpeaking(); };
  backend_api_.speech_voices = [](void*) -> char* {
    return laufey_common::SpeechVoicesJson();
  };
  backend_api_.get_background_throttling = Backend_GetBackgroundThrottling;
  backend_api_.is_visible = Backend_IsVisible;
  backend_api_.show = Backend_Show;
  backend_api_.hide = Backend_Hide;
  backend_api_.focus = Backend_Focus;
  backend_api_.post_ui_task = Backend_PostUiTask;

  laufey_register_value_api(&backend_api_);

  backend_api_.set_js_call_handler = Backend_SetJsCallHandler;
  backend_api_.js_call_respond = Backend_JsCallRespond;
  backend_api_.set_js_call_handler_ex = Backend_SetJsCallHandlerEx;
  backend_api_.platform_features = Backend_PlatformFeatures;
  backend_api_.tray_unavailable_reason = Backend_TrayUnavailableReason;
  backend_api_.set_platform_features_changed_handler =
      Backend_SetPlatformFeaturesChangedHandler;
  backend_api_.exit_app = Backend_ExitApp;
  backend_api_.set_quit_requested_handler = Backend_SetQuitRequestedHandler;
  backend_api_.answer_quit_request = Backend_AnswerQuitRequest;
  backend_api_.set_media_capture_handler = Backend_SetMediaCaptureHandler;
  backend_api_.answer_media_capture = Backend_AnswerMediaCapture;
  backend_api_.quit_request_pending = Backend_QuitRequestPending;
  // The navigation guard (API 48): every web-engine backend checks its
  // windows' top-level navigations (laufey_navigation_guard.h).
  // User activities: Handoff and Spotlight (API 49; macOS).
  backend_api_.set_user_activity = [](void*, const char* json) -> bool {
    return laufey_common::SetUserActivityForAbi(json);
  };
  backend_api_.set_user_activity_handler =
      [](void*, laufey_user_activity_fn handler, void* user_data) {
        laufey_common::SetUserActivityHandler(handler, user_data);
      };
  // Delivered notifications (API 49): backend-common's notification
  // platform (the notification center / toast history).
  backend_api_.list_delivered_notifications =
      [](void*, laufey_notification_list_fn cb, void* user_data) {
        laufey_common::ListDeliveredNotifications(cb, user_data);
      };
  backend_api_.remove_delivered_notifications =
      [](void*, const char* const* tags, size_t count, bool all) {
        laufey_common::RemoveDeliveredNotifications(tags, count, all);
      };
  backend_api_.set_allowed_navigation_origins =
      [](void*, const char* const* origins, size_t count) {
        laufey_common::SetAllowedNavigationOriginsForAbi(origins, count);
      };
  backend_api_.test_capture_external_opens = [](void*, bool capture) {
    laufey_common::TestCaptureExternalOpensForAbi(capture);
  };
  backend_api_.test_take_external_opens = [](void*) {
    return laufey_common::TestTakeExternalOpensForAbi();
  };
  backend_api_.title_bar_preferences = Backend_TitleBarPreferences;
  backend_api_.set_title_bar_preferences_changed_handler =
      Backend_SetTitleBarPreferencesChangedHandler;
#if defined(__linux__) || (defined(__APPLE__) && !TARGET_OS_IPHONE)
  // The secure store (API 47): the Secret Service through libsecret on
  // Linux, the Keychain (Security.framework, items only this app may read
  // without a prompt; see docs/secure-store.md) on macOS. Windows keeps NULL
  // (its Credential Locker is the embedder's).
  backend_api_.secret_lookup = [](void*, const char* service,
                                  const char* account, uint32_t timeout_ms,
                                  char** value, char** reason) {
    return laufey_common::SecretLookupForAbi(service, account, timeout_ms,
                                             value, reason);
  };
  backend_api_.secret_store =
      [](void*, const char* service, const char* account, const char* label,
         const char* value, uint32_t timeout_ms, char** reason) {
        return laufey_common::SecretStoreForAbi(service, account, label, value,
                                                timeout_ms, reason);
      };
  backend_api_.secret_delete = [](void*, const char* service,
                                  const char* account, uint32_t timeout_ms,
                                  char** reason) {
    return laufey_common::SecretDeleteForAbi(service, account, timeout_ms,
                                             reason);
  };
#endif

  backend_api_.register_scheme_handler = Backend_RegisterSchemeHandler;
  backend_api_.scheme_request_read_body = Backend_SchemeRequestReadBody;
  backend_api_.scheme_response_begin = Backend_SchemeResponseBegin;
  backend_api_.scheme_response_write = Backend_SchemeResponseWrite;
  backend_api_.scheme_response_finish = Backend_SchemeResponseFinish;

  backend_api_.invoke_js_callback = Backend_InvokeJsCallback;
  backend_api_.release_js_callback = Backend_ReleaseJsCallback;

  backend_api_.get_window_handle = [](void*, uint32_t) -> void* {
    return nullptr;
  };
  backend_api_.get_display_handle = [](void*, uint32_t) -> void* {
    return nullptr;
  };
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int {
    return LAUFEY_WINDOW_HANDLE_UNKNOWN;
  };

  backend_api_.set_keyboard_event_handler = Backend_SetKeyboardEventHandler;
  backend_api_.set_mouse_click_handler = Backend_SetMouseClickHandler;
  backend_api_.set_mouse_move_handler = Backend_SetMouseMoveHandler;
  backend_api_.set_wheel_handler = Backend_SetWheelHandler;
  backend_api_.set_cursor_enter_leave_handler =
      Backend_SetCursorEnterLeaveHandler;
  backend_api_.set_focused_handler = Backend_SetFocusedHandler;
  backend_api_.set_resize_handler = Backend_SetResizeHandler;
  backend_api_.set_move_handler = Backend_SetMoveHandler;
  backend_api_.poll_js_calls = Backend_PollJsCalls;
  backend_api_.set_js_call_notify = Backend_SetJsCallNotify;
  backend_api_.set_application_menu = Backend_SetApplicationMenu;
  backend_api_.show_context_menu = Backend_ShowContextMenu;
  backend_api_.open_devtools = Backend_OpenDevTools;
  backend_api_.print_to_pdf = Backend_PrintToPdf;
  backend_api_.set_js_namespace = Backend_SetJsNamespace;
  backend_api_.create_window = Backend_CreateWindow;
  backend_api_.create_window_ex = Backend_CreateWindowEx;
  backend_api_.close_window = Backend_CloseWindow;
  backend_api_.set_close_requested_handler = Backend_SetCloseRequestedHandler;
  backend_api_.test_trigger_close_requested = Backend_TestTriggerCloseRequested;
  backend_api_.test_inject_input = Backend_TestInjectInput;

  // Window state, constraints, screens and chrome (API >= 38): see
  // docs/window-management.md for what each OS / backend supports.
  backend_api_.set_window_state = Backend_SetWindowState;
  backend_api_.get_window_state = Backend_GetWindowState;
  backend_api_.set_window_state_handler = Backend_SetWindowStateHandler;
  backend_api_.set_window_size_constraints = Backend_SetWindowSizeConstraints;
  backend_api_.get_window_size_constraints = Backend_GetWindowSizeConstraints;
  backend_api_.get_screens = Backend_GetScreens;
  backend_api_.get_window_screen = Backend_GetWindowScreen;
  backend_api_.set_display_changed_handler = Backend_SetDisplayChangedHandler;
  backend_api_.window_capabilities = Backend_WindowCapabilities;
  backend_api_.set_window_titlebar_style = Backend_SetWindowTitlebarStyle;
  backend_api_.set_window_traffic_light_position =
      Backend_SetWindowTrafficLightPosition;
  backend_api_.set_window_backdrop = Backend_SetWindowBackdrop;
  backend_api_.get_window_normal_bounds = Backend_GetWindowNormalBounds;
  backend_api_.set_quit_on_last_window_closed =
      Backend_SetQuitOnLastWindowClosed;
  backend_api_.set_page_load_handler = Backend_SetPageLoadHandler;
  backend_api_.show_dialog = Backend_ShowDialog;
  backend_api_.string_free = Backend_StringFree;
  backend_api_.read_clipboard_text = Backend_ReadClipboardText;
  backend_api_.write_clipboard_text = Backend_WriteClipboardText;

  backend_api_.set_dock_badge = Backend_SetDockBadge;
  backend_api_.bounce_dock = Backend_BounceDock;
  backend_api_.set_dock_menu = Backend_SetDockMenu;
  backend_api_.set_dock_visible = Backend_SetDockVisible;
  backend_api_.set_dock_reopen_handler = Backend_SetDockReopenHandler;

  // Deep links are macOS-only (see set_open_url_handler in laufey.h). Leave
  // the pointers NULL elsewhere so an embedder can detect the absence rather
  // than register a handler that silently never fires.
#if defined(__APPLE__)
  backend_api_.set_open_url_handler = Backend_SetOpenUrlHandler;
  backend_api_.test_trigger_open_url = Backend_TestTriggerOpenUrl;
#endif

  // Single instance (API >= 36): see docs/deep-links.md. The desktop
  // backends implement it on every OS; iOS keeps the no-op default.
  backend_api_.set_second_instance_handler = Backend_SetSecondInstanceHandler;

  // Passkeys (API >= 37): see docs/passkeys.md. macOS and Windows run real
  // ceremonies; Linux and iOS answer not_supported.
  backend_api_.passkey_capabilities = Backend_PasskeyCapabilities;
  backend_api_.passkey_request = Backend_PasskeyRequest;
  // The share sheet (API 49).
  backend_api_.share = Backend_Share;

  // Drag and drop, file dialogs and the rich clipboard (API >= 39): see
  // docs/drag-and-drop.md, docs/file-dialogs.md, docs/clipboard.md. The
  // desktop backends implement them over backend-common; iOS keeps the
  // unsupported defaults.
  backend_api_.set_file_drop_handler = Backend_SetFileDropHandler;
  backend_api_.start_file_drag = Backend_StartFileDrag;
  backend_api_.test_trigger_file_drop = Backend_TestTriggerFileDrop;
  backend_api_.show_file_dialog = Backend_ShowFileDialog;
  backend_api_.cancel_file_dialog = Backend_CancelFileDialog;
  backend_api_.test_file_dialog_respond = Backend_TestFileDialogRespond;
  backend_api_.show_message_dialog = Backend_ShowMessageDialog;
  backend_api_.set_app_menu_click_handler = Backend_SetAppMenuClickHandler;
  // API 56: scheme request frames, will-navigate, the app's own menu.
  backend_api_.scheme_request_frame = Backend_SchemeRequestFrame;
  backend_api_.set_will_navigate_handler = Backend_SetWillNavigateHandler;
  backend_api_.answer_will_navigate = Backend_AnswerWillNavigate;
  backend_api_.set_app_menu = Backend_SetAppMenu;
  // Title visibility and toolbar style (API 57).
  backend_api_.set_window_title_visibility = Backend_SetWindowTitleVisibility;
  backend_api_.set_window_toolbar_style = Backend_SetWindowToolbarStyle;
  // Child web views (API 58).
  backend_api_.webview_capabilities = Backend_WebViewCapabilities;
  backend_api_.create_webview = Backend_CreateWebView;
  backend_api_.webview_set_bounds = Backend_WebViewSetBounds;
  backend_api_.webview_set_visible = Backend_WebViewSetVisible;
  backend_api_.webview_navigate = Backend_WebViewNavigate;
  backend_api_.webview_command = Backend_WebViewCommand;
  backend_api_.webview_state = Backend_WebViewState;
  backend_api_.webview_evaluate = Backend_WebViewEvaluate;
  backend_api_.webview_capture = Backend_WebViewCapture;
  backend_api_.webview_close = Backend_WebViewClose;
  backend_api_.webview_set_z_order = Backend_WebViewSetZOrder;
  backend_api_.webview_set_zoom = Backend_WebViewSetZoom;
  backend_api_.webview_get_zoom = Backend_WebViewGetZoom;
  backend_api_.webview_find = Backend_WebViewFind;
  backend_api_.webview_stop_find = Backend_WebViewStopFind;
  backend_api_.webview_open_devtools = Backend_WebViewOpenDevTools;
  backend_api_.webview_print = Backend_WebViewPrint;
  backend_api_.clipboard_capabilities = Backend_ClipboardCapabilities;
  backend_api_.read_clipboard_html = Backend_ReadClipboardHtml;
  backend_api_.write_clipboard_html = Backend_WriteClipboardHtml;
  backend_api_.read_clipboard_image = Backend_ReadClipboardImage;
  backend_api_.write_clipboard_image = Backend_WriteClipboardImage;
  backend_api_.read_clipboard_formats = Backend_ReadClipboardFormats;
  backend_api_.set_clipboard_change_handler = Backend_SetClipboardChangeHandler;
  backend_api_.buffer_free = Backend_BufferFree;

  // Global shortcuts, launch at login, DevTools (API >= 40): see
  // docs/global-shortcuts.md, docs/launch-at-login.md, docs/devtools.md. The
  // desktop backends implement them over backend-common; iOS keeps the
  // unsupported defaults.
  backend_api_.system_capabilities = Backend_SystemCapabilities;
  backend_api_.set_shortcut_handler = Backend_SetShortcutHandler;
  backend_api_.register_shortcut = Backend_RegisterShortcut;
  backend_api_.unregister_shortcut = Backend_UnregisterShortcut;
  backend_api_.unregister_all_shortcuts = Backend_UnregisterAllShortcuts;
  backend_api_.list_shortcuts = Backend_ListShortcuts;
  backend_api_.canonicalize_accelerator = Backend_CanonicalizeAccelerator;
  backend_api_.test_trigger_shortcut = Backend_TestTriggerShortcut;
  backend_api_.get_launch_at_login = Backend_GetLaunchAtLogin;
  backend_api_.set_launch_at_login = Backend_SetLaunchAtLogin;
  backend_api_.close_devtools = Backend_CloseDevTools;
  backend_api_.is_devtools_open = Backend_IsDevToolsOpen;
  backend_api_.is_devtools_enabled = Backend_IsDevToolsEnabled;

  backend_api_.create_tray_icon = Backend_CreateTrayIcon;
  backend_api_.destroy_tray_icon = Backend_DestroyTrayIcon;
  backend_api_.set_tray_icon = Backend_SetTrayIcon;
  backend_api_.set_tray_tooltip = Backend_SetTrayTooltip;
  backend_api_.set_tray_menu = Backend_SetTrayMenu;
  backend_api_.set_tray_click_handler = Backend_SetTrayClickHandler;
  backend_api_.set_tray_double_click_handler =
      Backend_SetTrayDoubleClickHandler;
  backend_api_.set_tray_icon_dark = Backend_SetTrayIconDark;
  backend_api_.get_tray_icon_bounds = Backend_GetTrayIconBounds;

  backend_api_.show_notification = Backend_ShowNotification;
  backend_api_.close_notification = Backend_CloseNotification;

  backend_api_.query_permission = Backend_QueryPermission;
  backend_api_.request_permission = Backend_RequestPermission;

  // Menus and notifications (API >= 41): see docs/menus.md and
  // docs/notifications.md. iOS keeps the unsupported defaults.
  backend_api_.menu_capabilities = Backend_MenuCapabilities;
  backend_api_.show_context_menu_ex = Backend_ShowContextMenuEx;
  backend_api_.test_dismiss_context_menu = Backend_TestDismissContextMenu;
  backend_api_.test_trigger_menu_accelerator =
      Backend_TestTriggerMenuAccelerator;
  backend_api_.notification_capabilities = Backend_NotificationCapabilities;
  backend_api_.set_notification_response_handler =
      Backend_SetNotificationResponseHandler;
  backend_api_.list_scheduled_notifications =
      Backend_ListScheduledNotifications;
  backend_api_.cancel_notification = Backend_CancelNotification;
  backend_api_.test_notification_respond = Backend_TestNotificationRespond;

  // UI-thread tasks and auth sessions (API >= 42): see docs/c-abi.md and
  // docs/auth-session.md. The dispatcher is bound to the UI thread in Load.
  backend_api_.dispatch_ui_task = Backend_DispatchUiTask;
  backend_api_.is_ui_thread = Backend_IsUiThread;
  backend_api_.auth_session_capabilities = Backend_AuthSessionCapabilities;
  backend_api_.auth_session_start = Backend_AuthSessionStart;
  backend_api_.test_cancel_auth_session = Backend_TestCancelAuthSession;
  backend_api_.auth_session_cancel = Backend_AuthSessionCancel;
}

RuntimeLoader::RuntimeLoader() {
  instance_ = this;
  InitializeBackendApi();
}

RuntimeLoader::~RuntimeLoader() {
  Shutdown();
  if (library_handle_) {
#ifndef _WIN32
    dlclose(library_handle_);
#else
    FreeLibrary(static_cast<HMODULE>(library_handle_));
#endif
  }
  instance_ = nullptr;
}

RuntimeLoader* RuntimeLoader::GetInstance() {
  if (!instance_) {
    instance_ = new RuntimeLoader();
  }
  return instance_;
}

bool RuntimeLoader::Load(const std::string& path) {
  // Load runs on the UI thread (each host's main thread, which runs the
  // backend's loop): bind dispatch_ui_task's queue to it before the runtime
  // can dispatch anything.
  laufey_common::UiTaskDispatcher::Get().Bind(
      [this](void (*task)(void*), void* task_data) {
        LaufeyBackend* backend = GetBackend();
        if (!backend)
          return false;  // a headless worker has no UI thread
        return backend->PostUiTask(task, task_data);
      });
#if defined(__linux__)
  // Deno.exit() ends the process from the runtime thread: keep the UI thread
  // from drawing while exit tears the libraries down.
  laufey_common::InstallUiExitGuard();
#endif
#ifndef _WIN32
  library_handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library_handle_) {
    std::cerr << "Failed to load runtime: " << dlerror() << std::endl;
    return false;
  }

  init_fn_ = reinterpret_cast<laufey_runtime_init_fn>(
      dlsym(library_handle_, LAUFEY_RUNTIME_INIT_SYMBOL));
  if (!init_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_INIT_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }

  start_fn_ = reinterpret_cast<laufey_runtime_start_fn>(
      dlsym(library_handle_, LAUFEY_RUNTIME_START_SYMBOL));
  if (!start_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_START_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }

  shutdown_fn_ = reinterpret_cast<laufey_runtime_shutdown_fn>(
      dlsym(library_handle_, LAUFEY_RUNTIME_SHUTDOWN_SYMBOL));
  if (!shutdown_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_SHUTDOWN_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }
#else
  library_handle_ = LoadLibraryW(laufey_common::Utf8ToWide(path).c_str());
  if (!library_handle_) {
    std::cerr << "Failed to load runtime: " << GetLastError() << std::endl;
    return false;
  }

  init_fn_ = reinterpret_cast<laufey_runtime_init_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), LAUFEY_RUNTIME_INIT_SYMBOL));
  if (!init_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_INIT_SYMBOL << std::endl;
    return false;
  }

  start_fn_ = reinterpret_cast<laufey_runtime_start_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), LAUFEY_RUNTIME_START_SYMBOL));
  if (!start_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_START_SYMBOL << std::endl;
    return false;
  }

  shutdown_fn_ = reinterpret_cast<laufey_runtime_shutdown_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), LAUFEY_RUNTIME_SHUTDOWN_SYMBOL));
  if (!shutdown_fn_) {
    std::cerr << "Failed to find " << LAUFEY_RUNTIME_SHUTDOWN_SYMBOL
              << std::endl;
    return false;
  }
#endif

  // stderr: stdout is the app's own (a headless worker's or a sidecar's
  // output, which its parent reads).
  std::cerr << "Runtime loaded successfully from: " << path << std::endl;
  return true;
}

bool RuntimeLoader::Start() {
  if (running_) {
    return true;
  }

  if (!init_fn_ || !start_fn_) {
    std::cerr << "Runtime not loaded" << std::endl;
    return false;
  }

  int result = init_fn_(&backend_api_);
  if (result != 0) {
    std::cerr << "Runtime init failed with code: " << result << std::endl;
    return false;
  }

  running_ = true;
  runtime_thread_ = std::thread(&RuntimeLoader::RuntimeThread, this);

  std::cerr << "Runtime started" << std::endl;
  return true;
}

void RuntimeLoader::RuntimeThread() {
  int result = start_fn_();
  if (result != 0) {
    std::cerr << "Runtime start returned error: " << result << std::endl;
  }
  running_ = false;
  runtime_exit_.Done();
}

void RuntimeLoader::Shutdown() {
  if (shutdown_fn_) {
    shutdown_fn_();
  }

  if (runtime_thread_.joinable()) {
    // The app asked to exit (exit_app): its thread may never return (an
    // exit() that blocks for good, as Deno.exit()), and the process ends
    // with the code it asked for without it. A runtime that does return is
    // given a moment to.
    if (laufey_common::ExitRequested() &&
        !runtime_exit_.WaitFor(kRuntimeExitGrace)) {
      runtime_thread_.detach();
      return;
    }
    // The loop has ended (UiLoopEnded), so the runtime's synchronous UI calls
    // already return. A runtime that still ignores the shutdown is abandoned
    // after the timeout, as on Winit, rather than hanging the exit.
    if (runtime_exit_.WaitFor(kRuntimeShutdownTimeout)) {
      runtime_thread_.join();
    } else {
      std::cerr << "laufey: the runtime did not stop within "
                << kRuntimeShutdownTimeout.count()
                << " ms of shutdown; exiting without it" << std::endl;
      runtime_thread_.detach();
    }
  }
}

void RuntimeLoader::SetSchemeRequestHandler(const std::string& scheme,
                                            laufey_scheme_request_fn handler,
                                            laufey_scheme_cancel_fn on_cancel,
                                            void* user_data) {
  {
    std::lock_guard<std::mutex> lock(scheme_mutex_);
    scheme_request_handler_ = handler;
    scheme_cancel_handler_ = on_cancel;
    scheme_user_data_ = user_data;
  }
  if (handler && backend_) {
    backend_->RegisterSchemeHandler(scheme);
  }
}

void RuntimeLoader::DispatchSchemeCancel(SchemeExchangeBase* exchange) {
  laufey_scheme_cancel_fn on_cancel;
  void* user_data;
  {
    std::lock_guard<std::mutex> lock(scheme_mutex_);
    on_cancel = scheme_cancel_handler_;
    user_data = scheme_user_data_;
  }
  if (on_cancel)
    on_cancel(user_data, reinterpret_cast<laufey_scheme_exchange_t*>(exchange));
}

void RuntimeLoader::DispatchSchemeRequest(uint32_t window_id,
                                          SchemeExchangeBase* exchange,
                                          const std::string& method,
                                          const std::string& url,
                                          const std::string& flat_headers) {
  laufey_scheme_request_fn handler;
  void* user_data;
  {
    std::lock_guard<std::mutex> lock(scheme_mutex_);
    handler = scheme_request_handler_;
    user_data = scheme_user_data_;
  }
  if (handler) {
    handler(user_data, window_id,
            reinterpret_cast<laufey_scheme_exchange_t*>(exchange),
            method.c_str(), url.c_str(), flat_headers.data(),
            flat_headers.size());
  } else {
    // No handler registered: finish so the request doesn't hang.
    exchange->Finish();
  }
}

const laufey_common::BridgeOriginPolicy& RuntimeLoader::BridgePolicy() const {
#if defined(__APPLE__) && TARGET_OS_IPHONE
  // The iOS shell has no launch file.
  static const laufey_common::BridgeOriginPolicy unrestricted;
  return unrestricted;
#else
  return laufey_common::ProcessBridgeOriginPolicy();
#endif
}

void RuntimeLoader::OnJsCall(uint32_t window_id, uint64_t page_call_id,
                             const std::string& method_path,
                             laufey::ValuePtr args, const std::string& origin) {
  uint64_t call_id = js_calls_.Add(window_id, page_call_id);
  // A document the launch file's bridge pin doesn't cover never reaches the
  // runtime (its bridge script isn't installed either; this catches a page
  // that posts to the engine's message channel itself).
  if (!laufey_common::BridgeOriginAllowed(BridgePolicy(), origin)) {
    std::cerr << "laufey: refused a bridge call from " << origin
              << " (not in the app's bridgeOrigins)" << std::endl;
    RespondToCall(call_id, nullptr,
                  laufey::Value::String(laufey_common::kBridgeOriginRefused));
    return;
  }
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_js_calls_.push({window_id, call_id, method_path, args, origin});
  }

  std::lock_guard<std::mutex> lock(notify_mutex_);
  if (js_call_notify_fn_) {
    js_call_notify_fn_(js_call_notify_data_);
  }
}

void RuntimeLoader::PollPendingJsCalls() {
  std::vector<PendingJsCall> calls;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    while (!pending_js_calls_.empty()) {
      calls.push_back(std::move(pending_js_calls_.front()));
      pending_js_calls_.pop();
    }
  }

  if (calls.empty())
    return;

  laufey_js_call_fn handler;
  void* user_data;
  laufey_js_call_ex_fn handler_ex;
  void* user_data_ex;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler = js_call_handler_;
    user_data = js_call_user_data_;
    handler_ex = js_call_handler_ex_;
    user_data_ex = js_call_user_data_ex_;
  }

  for (auto& call : calls) {
    // Reserved bridge call from the injected external-link interceptor
    // (laufey_external_links.h): open the URL in the OS browser instead of
    // forwarding to the runtime, then resolve the page-side promise.
    if (call.method_path == LAUFEY_OPEN_EXTERNAL_METHOD) {
      std::string url;
      if (call.args && call.args->IsList()) {
        const laufey::ValueList& list = call.args->GetList();
        if (!list.empty() && list[0] && list[0]->IsString()) {
          url = list[0]->GetString();
        }
      }
      // An origin the navigation guard lets this window load (one the
      // embedder allows) stays in the window; anything else goes to the OS
      // browser when the URL rule allows it, within the window's
      // per-second budget.
      laufey_common::NavigationDecision decision =
          laufey_common::NavigationGuard::Instance().Decide(call.window_id,
                                                            url);
      if (LaufeyBackend* backend = GetBackend()) {
        if (decision == laufey_common::NavigationDecision::kAllow) {
          backend->Navigate(call.window_id, url);
        } else if (decision ==
                   laufey_common::NavigationDecision::kOpenInBrowser) {
          backend->OpenExternalURL(url);
        } else {
          laufey_common::LogGuardedNavigation(url, decision);
        }
      }
      RespondToCall(call.call_id, laufey::Value::Null(), nullptr);
      continue;
    }

    // Reserved bridge call from WebView2's download-link script
    // (scheme_download.h): save the app-scheme URL through the scheme
    // handler, after the download policy (the calling document's origin is
    // the initiator) and the save dialog.
    if (call.method_path == LAUFEY_SCHEME_DOWNLOAD_METHOD) {
      std::string url;
      std::string name;
      if (call.args && call.args->IsList()) {
        const laufey::ValueList& list = call.args->GetList();
        if (!list.empty() && list[0] && list[0]->IsString())
          url = list[0]->GetString();
        if (list.size() > 1 && list[1] && list[1]->IsString())
          name = list[1]->GetString();
      }
      LaufeyBackend* backend = GetBackend();
      if (backend && laufey_common::IsAppSchemeUrl(url)) {
        uint32_t wid = call.window_id;
        laufey_webview::StartSchemeDownload(
            wid, url, call.origin, "GET", "",
            laufey_common::SchemeDownloadName(url, "", name),
            [backend, wid](const laufey_file_dialog_options_t* options,
                           laufey_file_dialog_result_fn callback,
                           void* user_data) {
              return backend->ShowFileDialog(wid, options, callback, user_data);
            },
            [](std::function<void()> fn) { fn(); });
      }
      RespondToCall(call.call_id, laufey::Value::Null(), nullptr);
      continue;
    }

    if (handler_ex) {
      laufey_value_t* argsWrapper = new laufey_value(call.args);
      handler_ex(user_data_ex, call.window_id, call.call_id,
                 call.method_path.c_str(), argsWrapper, call.origin.c_str());
    } else if (handler) {
      laufey_value_t* argsWrapper = new laufey_value(call.args);
      handler(user_data, call.window_id, call.call_id, call.method_path.c_str(),
              argsWrapper);
    } else {
      RespondToCall(call.call_id, nullptr,
                    laufey::Value::String("No JS call handler registered"));
    }
  }
}

void RuntimeLoader::RespondToCall(uint64_t call_id, laufey::ValuePtr result,
                                  laufey::ValuePtr error) {
  laufey_common::JsCallRoute route;
  if (js_calls_.Take(call_id, &route))
    JsCallRespond(route.window_id, route.page_call_id, result, error);
}

void RuntimeLoader::JsCallRespond(uint32_t window_id, uint64_t call_id,
                                  laufey::ValuePtr result,
                                  laufey::ValuePtr error) {
  LaufeyBackend* backend = GetBackend();
  if (backend) {
    backend->RespondToJsCall(window_id, call_id, result, error);
  }
}
