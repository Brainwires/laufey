// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Child web views on WebView2 (API 58): a second ICoreWebView2Controller laid
// over a window's page controller, in a profile of its own. Every function
// but Capabilities / NoteEnvironment runs on the UI thread (WebView2Backend
// hops there). See docs/webviews.md.
//
// Creation is asynchronous: Create registers the view (its id, its host
// window at the bounds) and returns at once; Start creates its controller
// once the window's own web view (and so its environment) is ready. Calls
// made before the controller exists are kept (bounds, visibility, the URL)
// or answered "not ready" (evaluate, capture).

#ifndef LAUFEY_CHILD_WEBVIEW_WIN_H_
#define LAUFEY_CHILD_WEBVIEW_WIN_H_

#include <windows.h>
// WebView2.h needs the COM / OLE declarations wrl.h brings in (as in
// webview_windows.cc).
#include <wrl.h>

#include <cstdint>
#include <string>
#include <vector>

#include "WebView2.h"
#include "laufey.h"
#include "laufey_webview.h"

namespace laufey_child_webview_win {

// LAUFEY_WEBVIEW_CAP_* plus the engine, or 0 when the WebView2 runtime has no
// profiles (ICoreWebView2Environment10). Any thread.
uint32_t Capabilities();

// Notes whether a window's environment has profiles, which then decides
// Capabilities. Any thread.
void NoteEnvironment(ICoreWebView2Environment* env);

// Whether views can be created from `env` (it has
// ICoreWebView2Environment10).
bool SupportsEnvironment(ICoreWebView2Environment* env);

// Registers a view over window `window_id` (its HWND `parent`) and returns its
// id, or 0 with `*status_out` set (NO_WINDOW, LIMIT, FAILED). No event fires
// before Start.
uint32_t Create(uint32_t window_id, HWND parent,
                const laufey_common::WebViewOptions& options,
                laufey_webview_event_fn on_event, void* user_data,
                int32_t* status_out);

// The window's web view is ready: creates the view's controller from `env`
// (the window's environment), laid out against `page` (the window's page
// controller). S_OK when started, or when the view is gone; otherwise the
// error, and the caller drops the view (Abandon) or fails it (Fail).
HRESULT Start(uint32_t id, ICoreWebView2Environment* env,
              ICoreWebView2Controller* page);

// The view could not be created: a LOAD "failed" event, then CLOSED.
void Fail(uint32_t id, const std::string& message);

// Forgets a view Create returned, without any event (its creation was refused
// before create_webview returned).
void Abandon(uint32_t id);

bool SetBounds(uint32_t id, double x, double y, double width, double height);
bool SetVisible(uint32_t id, bool visible);
int32_t Navigate(uint32_t id, const std::string& url);
bool Command(uint32_t id, int32_t command);
// Empty for an unknown id.
std::string State(uint32_t id);
void Evaluate(uint32_t id, const std::string& script,
              laufey_webview_eval_fn callback, void* user_data);
// `scale`: output pixels per CSS pixel; 0: the display's device pixels.
void Capture(uint32_t id, int32_t format, int32_t quality, double scale,
             laufey_webview_capture_fn callback, void* user_data);
// Orders the window's views bottom to top.
bool Restack(const std::vector<uint32_t>& bottom_to_top);
bool Close(uint32_t id);
// Replaces the view's "forwardKeys" (canonical forms).
bool SetForwardKeys(uint32_t id, const std::vector<std::string>& keys);

// The window resized or changed its scale: lays its views out again.
void RelayoutForWindow(uint32_t window_id);
// The window is closing: closes its views (each fires CLOSED).
void CloseAllForWindow(uint32_t window_id);

}  // namespace laufey_child_webview_win

#endif  // LAUFEY_CHILD_WEBVIEW_WIN_H_
