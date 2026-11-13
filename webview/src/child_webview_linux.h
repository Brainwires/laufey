// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Child web views on WebKitGTK (API 58): a second WebKitWebView laid over a
// window's page WebKitWebView, as a child of the GtkOverlay that holds the
// page. Every function but Capabilities runs on the GTK thread
// (WebKitGTKBackend hops there). See docs/webviews.md.

#ifndef LAUFEY_CHILD_WEBVIEW_LINUX_H_
#define LAUFEY_CHILD_WEBVIEW_LINUX_H_

#include <cstdint>
#include <string>
#include <vector>

#include "laufey.h"
#include "laufey_webview.h"

// GTK's own type, declared here so this header needs no GTK include.
typedef struct _GtkWidget GtkWidget;

namespace laufey_child_webview_linux {

// LAUFEY_WEBVIEW_CAP_* plus the engine. Any thread.
uint32_t Capabilities();

// Creates a view in `overlay` (the window's GtkOverlay*, whose main child is
// the page WebKitWebView) in `window_id`. Returns the id, or 0 with
// `*status_out` set.
uint32_t Create(uint32_t window_id, void* overlay,
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
// Orders the window's views bottom to top.
bool Restack(const std::vector<uint32_t>& bottom_to_top);
bool Close(uint32_t id);
// Replaces the view's "forwardKeys" (canonical forms).
bool SetForwardKeys(uint32_t id, const std::vector<std::string>& keys);
// The window is closing: close its views (each fires CLOSED).
void CloseAllForWindow(uint32_t window_id);
// Whether `widget` is a child web view or inside one: its keys belong to that
// view's page, never to the window's own keyboard events. GTK thread.
bool OwnsWidget(GtkWidget* widget);

}  // namespace laufey_child_webview_linux

#endif  // LAUFEY_CHILD_WEBVIEW_LINUX_H_
