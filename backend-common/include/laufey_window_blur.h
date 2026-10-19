// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// KWin's blur behind translucent windows (Linux), the backdrop
// set_window_backdrop puts behind a WebKitGTK window on KDE Plasma
// (LAUFEY_BACKDROP_BLUR, and LAUFEY_BACKDROP_ACRYLIC as the same blur).
// See docs/window-management.md.
//
//   Wayland  the org_kde_kwin_blur_manager global (KDE's blur protocol):
//            create(surface) -> org_kde_kwin_blur, set_region(null) = the
//            whole surface, commit; applied with the surface's next commit.
//   X11      the _KDE_NET_WM_BLUR_BEHIND_REGION property on the client
//            window: CARDINAL/32, empty = the whole window; removed to stop.
//
// Detected by feature probing, never by the desktop's name: KWin offers the
// global (Wayland) or lists the atom in the root window's _NET_SUPPORTED
// (X11) only while its Blur effect is loaded, so another compositor, or
// KWin with the effect off (or without OpenGL compositing), reports none.
// The protocol's interface tables are written here from its wire
// description; no generated code.

#ifndef LAUFEY_WINDOW_BLUR_H_
#define LAUFEY_WINDOW_BLUR_H_

#include <cstdint>
#include <string>

struct wl_display;
struct wl_surface;
struct wl_proxy;
struct xcb_connection_t;

namespace laufey_common {

// --- Wayland ----------------------------------------------------------------
//
// `display` is the connection the surfaces belong to (GTK's). Its registry
// is read on a private event queue, so the caller's own dispatching is left
// alone. Call all of these from one thread per display (the UI thread).

// Whether the compositor offers org_kde_kwin_blur_manager now: a round trip
// on the private queue, so a global KWin added or removed since (the effect
// turned on or off) counts at once.
bool KwinBlurWaylandAvailable(wl_display* display);

// Blur behind the whole of `surface` (on) or stop (off). `*blur` is the
// surface's org_kde_kwin_blur object: created when null, released for off.
// Takes effect with the surface's next commit (the caller has the toolkit
// draw a frame). False when the compositor offers no blur manager (on), or
// for a null display / surface. Off without a manager just drops `*blur`.
bool KwinBlurWaylandSet(wl_display* display, wl_surface* surface,
                        wl_proxy** blur, bool on);

// Drops `*blur` (the surface is going away: GTK destroys the wl_surface of
// a window it unmaps) without asking the compositor to unset anything.
void KwinBlurWaylandForget(wl_proxy** blur);

// --- X11 --------------------------------------------------------------------
//
// Over a private XCB connection to $DISPLAY (the server GTK's windows are
// on), opened on first use. Any thread.

// Whether the root window's _NET_SUPPORTED lists
// _KDE_NET_WM_BLUR_BEHIND_REGION (KWin with its Blur effect loaded). Read
// on every call: the window manager may be replaced, or the effect toggled.
bool KwinBlurX11Available();

// Sets (on) or deletes (off) _KDE_NET_WM_BLUR_BEHIND_REGION on `window`.
// On is refused (false) where KwinBlurX11Available() is false.
bool KwinBlurX11Set(uint32_t window, bool on);

// The same on a connection and root window of the caller's (tests).
bool KwinBlurX11AvailableOn(xcb_connection_t* conn, uint32_t root);
bool KwinBlurX11SetOn(xcb_connection_t* conn, uint32_t root, uint32_t window,
                      bool on);

// --- The session (platform_features) ----------------------------------------

// What this session's compositor offers, on a connection of its own to the
// display this process uses (DisplayBackend): "kwin-wayland", "kwin-x11", or
// "" with `*reason` saying why not. Linux only ("" elsewhere).
std::string ProbeWindowBlur(std::string* reason);

}  // namespace laufey_common

#endif  // LAUFEY_WINDOW_BLUR_H_
