// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Child web views on WKWebView (API 58): a second WKWebView laid over a
// window's page WKWebView. Every function but Capabilities runs on the main
// thread (WKWebViewBackend hops there). See docs/webviews.md.

#ifndef LAUFEY_CHILD_WEBVIEW_MAC_H_
#define LAUFEY_CHILD_WEBVIEW_MAC_H_

#include <cstdint>
#include <string>

#include "laufey.h"
#include "laufey_webview.h"

namespace laufey_child_webview_mac {

// LAUFEY_WEBVIEW_CAP_* plus the engine. Any thread.
uint32_t Capabilities();

// Creates a view over `page_view` (the window's page WKWebView, an NSView*)
// in `window_id`. Returns the id, or 0 with `*status_out` set.
uint32_t Create(uint32_t window_id, void* page_view,
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
void Capture(uint32_t id, int32_t format, int32_t quality,
             laufey_webview_capture_fn callback, void* user_data);
bool Close(uint32_t id);
// The window is closing: close its views (each fires CLOSED).
void CloseAllForWindow(uint32_t window_id);

}  // namespace laufey_child_webview_mac

#endif  // LAUFEY_CHILD_WEBVIEW_MAC_H_
