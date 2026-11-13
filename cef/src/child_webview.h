// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Child web views on CEF (API 58): an Alloy-style CefBrowserView laid over a
// window as a CEF Views overlay (CefWindow::AddOverlayView, custom docking),
// with a CefClient of its own (child_webview.cc) and a CefRequestContext per
// partition. See docs/webviews.md.
//
// Every function but Capabilities runs on the CEF UI thread (runtime_loader.cc
// hops there).

#ifndef LAUFEY_CEF_CHILD_WEBVIEW_H_
#define LAUFEY_CEF_CHILD_WEBVIEW_H_

#include <cstdint>
#include <string>
#include <vector>

#include "include/cef_browser.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "laufey.h"
#include "laufey_webview.h"

namespace laufey_cef_child_webview {

// The key a child browser's extra_info carries, so the renderer never gives
// it the bridge (render_process_handler.cc).
constexpr const char kExtraInfoKey[] = "laufey_child_webview";

// LAUFEY_WEBVIEW_CAP_* plus the engine. Any thread.
uint32_t Capabilities();

// Creates a view in `window` (the laufey window `window_id`) over
// `page_view` (its page's browser view). Returns the id, or 0 with
// `*status_out` set.
uint32_t Create(uint32_t window_id, CefRefPtr<CefWindow> window,
                CefRefPtr<CefBrowserView> page_view,
                const laufey_common::WebViewOptions& options,
                laufey_webview_event_fn on_event, void* user_data,
                int32_t* status_out);
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
// Orders the window's views bottom to top (macOS only; false elsewhere).
bool Restack(uint32_t window_id, const std::vector<uint32_t>& bottom_to_top);
bool Close(uint32_t id);
// The window is closing: close its views (each fires CLOSED).
void CloseAllForWindow(uint32_t window_id);

}  // namespace laufey_cef_child_webview

// Shows a message dialog for window `window_id` (runtime_loader.cc's
// show_message_dialog), for "permissions": "ask".
uint32_t LaufeyShowMessageDialogForWindow(
    uint32_t window_id, const laufey_message_dialog_options_t* options,
    laufey_message_dialog_result_fn callback, void* user_data);

#if defined(__APPLE__)
// runtime_loader_mac.mm: orders the overlay windows holding `browser_views`
// (each a browser's NSView) above the window `window_id`, bottom to top.
bool LaufeyCefRestackOverlaysMac(uint32_t window_id,
                                 const std::vector<void*>& browser_views);
#endif

#endif  // LAUFEY_CEF_CHILD_WEBVIEW_H_
