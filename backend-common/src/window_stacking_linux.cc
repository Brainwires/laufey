// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The stacking order of the app's windows and the pointer on Linux X11
// (API 60: get_window_z_order / get_pointer_state), shared by the WebKitGTK
// and CEF backends. See laufey_window.h.
//
// Asked of the X server over a connection of this file's own (XCB, where an
// error comes back with its request: no process-wide Xlib error handler to
// swap, and no dependence on which toolkit drew the window, GTK or
// Chromium). Nothing here works under Wayland, by the protocol's design: a
// client is told neither how surfaces are stacked nor where the pointer is
// outside its own surfaces, and XWayland knows only about X11 windows.

#include <xcb/xcb.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "laufey_platform_features.h"
#include "laufey_window.h"

namespace laufey_common {

namespace {

std::mutex g_mutex;
xcb_connection_t* g_conn = nullptr;
xcb_window_t g_root = XCB_WINDOW_NONE;

// The connection, opened once (and again after the server dropped it).
// g_mutex held.
bool Connect() {
  if (g_conn && !xcb_connection_has_error(g_conn))
    return true;
  if (g_conn) {
    xcb_disconnect(g_conn);
    g_conn = nullptr;
  }
  if (DisplayBackend() != "x11")
    return false;
  int screen_num = 0;
  xcb_connection_t* conn = xcb_connect(nullptr, &screen_num);
  if (!conn || xcb_connection_has_error(conn)) {
    if (conn)
      xcb_disconnect(conn);
    return false;
  }
  xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
  for (int i = 0; i < screen_num && it.rem; ++i)
    xcb_screen_next(&it);
  if (!it.data) {
    xcb_disconnect(conn);
    return false;
  }
  g_conn = conn;
  g_root = it.data->root;
  return true;
}

// The root window's child that holds `window` (the window manager's frame,
// or the window itself when it was not reparented); 0 when the window is
// gone. A window manager nests a client a few levels deep at most.
xcb_window_t TopLevelOf(xcb_window_t window) {
  xcb_window_t current = window;
  for (int depth = 0; depth < 32 && current != XCB_WINDOW_NONE; ++depth) {
    xcb_query_tree_reply_t* tree =
        xcb_query_tree_reply(g_conn, xcb_query_tree(g_conn, current), nullptr);
    if (!tree)
      return XCB_WINDOW_NONE;
    xcb_window_t parent = tree->parent;
    xcb_window_t root = tree->root;
    std::free(tree);
    if (parent == root || parent == XCB_WINDOW_NONE)
      return root == g_root ? current : XCB_WINDOW_NONE;
    current = parent;
  }
  return XCB_WINDOW_NONE;
}

bool Viewable(xcb_window_t window) {
  xcb_get_window_attributes_reply_t* attrs = xcb_get_window_attributes_reply(
      g_conn, xcb_get_window_attributes(g_conn, window), nullptr);
  if (!attrs)
    return false;
  bool viewable = attrs->map_state == XCB_MAP_STATE_VIEWABLE;
  std::free(attrs);
  return viewable;
}

// `windows` with each handle replaced by its viewable top-level ancestor (0
// for a window that is unmapped: hidden, minimized, on another workspace).
std::vector<NativeWindow> TopLevels(const std::vector<NativeWindow>& windows) {
  std::vector<NativeWindow> tops;
  for (const NativeWindow& w : windows) {
    NativeWindow top;
    top.id = w.id;
    xcb_window_t own = static_cast<xcb_window_t>(w.handle);
    if (own != XCB_WINDOW_NONE && Viewable(own)) {
      xcb_window_t frame = TopLevelOf(own);
      if (frame != XCB_WINDOW_NONE && Viewable(frame))
        top.handle = frame;
    }
    tops.push_back(top);
  }
  return tops;
}

}  // namespace

bool LinuxX11StackingAvailable() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return Connect();
}

std::vector<uint32_t> LinuxX11WindowZOrder(
    const std::vector<NativeWindow>& windows) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Connect())
    return {};
  xcb_query_tree_reply_t* tree =
      xcb_query_tree_reply(g_conn, xcb_query_tree(g_conn, g_root), nullptr);
  if (!tree)
    return {};
  // The root's children come bottom-most first.
  std::vector<uintptr_t> stacking;
  xcb_window_t* children = xcb_query_tree_children(tree);
  int count = xcb_query_tree_children_length(tree);
  for (int i = count - 1; i >= 0; --i)
    stacking.push_back(children[i]);
  std::free(tree);
  return StackedWindowIds(stacking, TopLevels(windows));
}

bool LinuxX11PointerState(const std::vector<NativeWindow>& windows,
                          laufey_pointer_state_t* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!Connect())
    return false;
  xcb_query_pointer_reply_t* pointer = xcb_query_pointer_reply(
      g_conn, xcb_query_pointer(g_conn, g_root), nullptr);
  if (!pointer)
    return false;
  bool same_screen = pointer->same_screen;
  xcb_window_t child = pointer->child;
  int16_t x = pointer->root_x;
  int16_t y = pointer->root_y;
  uint16_t mask = pointer->mask;
  std::free(pointer);
  if (!same_screen)
    return false;
  out->x = x;
  out->y = y;
  // X numbers the buttons left, middle, right; a left-handed mapping
  // (xmodmap, the desktop's setting) is applied by the server.
  out->buttons = 0;
  if (mask & XCB_KEY_BUT_MASK_BUTTON_1)
    out->buttons |= LAUFEY_POINTER_BUTTON_PRIMARY;
  if (mask & XCB_KEY_BUT_MASK_BUTTON_3)
    out->buttons |= LAUFEY_POINTER_BUTTON_SECONDARY;
  if (mask & XCB_KEY_BUT_MASK_BUTTON_2)
    out->buttons |= LAUFEY_POINTER_BUTTON_AUXILIARY;
  // `child` is the root's child under the pointer: the frame (of any
  // client) that is topmost there.
  out->window_id = child == XCB_WINDOW_NONE
                       ? 0
                       : WindowIdForHandle(TopLevels(windows), child);
  return true;
}

}  // namespace laufey_common
