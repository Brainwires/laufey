// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Child web views on WKWebView (API 58): a second WKWebView laid over a
// window's page WKWebView. Every function but Capabilities runs on the main
// thread (WKWebViewBackend hops there). See docs/webviews.md.

#ifndef LAUFEY_CHILD_WEBVIEW_MAC_H_
#define LAUFEY_CHILD_WEBVIEW_MAC_H_

#include <cstdint>
#include <string>
#include <vector>

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
// `scale`: output pixels per CSS pixel; 0: the display's device pixels.
void Capture(uint32_t id, int32_t format, int32_t quality, double scale,
             laufey_webview_capture_fn callback, void* user_data);
// Orders the window's views bottom to top (the registry's stack order).
bool Restack(const std::vector<uint32_t>& bottom_to_top);
bool Close(uint32_t id);
// Page zoom (pageZoom); GetZoom answers 0 for an unknown id.
bool SetZoom(uint32_t id, double factor);
double GetZoom(uint32_t id);
// Find in page (findString:withConfiguration:) with a FIND_RESULT event;
// StopFindInPage clears the match.
bool FindInPage(uint32_t id, const std::string& text, uint32_t flags);
bool StopFindInPage(uint32_t id);
// OK, NOT_ALLOWED (the app's DevTools are off), CLOSED or UNSUPPORTED.
int32_t OpenDevTools(uint32_t id);
// The OS print dialog, as a sheet on the window (returns once it started).
bool Print(uint32_t id);
// Replaces the view's "forwardKeys" (canonical forms).
bool SetForwardKeys(uint32_t id, const std::vector<std::string>& keys);
// The window is closing: close its views (each fires CLOSED).
void CloseAllForWindow(uint32_t window_id);

}  // namespace laufey_child_webview_mac

#endif  // LAUFEY_CHILD_WEBVIEW_MAC_H_
