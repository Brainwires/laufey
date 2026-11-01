// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Overlay windows on Linux (API 50); see laufey_overlay.h. Input shapes,
// the skip-taskbar / sticky / keep-above hints through GDK (X11 and
// Wayland), and wlr-layer-shell through a dlopen'd gtk-layer-shell.

#include "laufey_overlay.h"

#include <dlfcn.h>
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#include <wayland-client.h>
#endif

#include <cmath>
#include <cstring>
#include <map>
#include <string>

namespace laufey_common {

namespace {

struct OverlayInputState {
  bool explicit_passthrough = false;
  InputRegion region;
  double scale = 1.0;  // the region's, as last set
};

// Touched on the UI thread only.
std::map<uint64_t, OverlayInputState>& InputStates() {
  static auto* map = new std::map<uint64_t, OverlayInputState>();
  return *map;
}

void ApplyInputShape(void* gtk_window, void* gdk_window,
                     const OverlayInputState& st, double scale) {
  cairo_region_t* shape = nullptr;
  if (st.explicit_passthrough) {
    shape = cairo_region_create();
  } else if (st.region.active) {
    shape = cairo_region_create();
    if (!(scale > 0) || !std::isfinite(scale))
      scale = 1.0;
    for (const laufey_rect_t& r : st.region.rects) {
      cairo_rectangle_int_t rect = {
          (int)std::floor(r.x * scale), (int)std::floor(r.y * scale),
          (int)std::ceil(r.width * scale), (int)std::ceil(r.height * scale)};
      cairo_region_union_rectangle(shape, &rect);
    }
  }
  // NULL restores the default input shape (the whole window).
  if (gtk_window) {
    gtk_widget_input_shape_combine_region(GTK_WIDGET(gtk_window), shape);
  } else if (gdk_window) {
    gdk_window_input_shape_combine_region(GDK_WINDOW(gdk_window), shape, 0, 0);
  }
  if (shape)
    cairo_region_destroy(shape);
}

// --- gtk-layer-shell (GTK3), dlopen'd ---------------------------------------

// The enums of gtk-layer-shell.h, stable since 0.1.
constexpr int kLayerOverlay = 3;      // GTK_LAYER_SHELL_LAYER_OVERLAY
constexpr int kEdgeLeft = 0;          // GTK_LAYER_SHELL_EDGE_LEFT
constexpr int kEdgeRight = 1;         // GTK_LAYER_SHELL_EDGE_RIGHT
constexpr int kEdgeTop = 2;           // GTK_LAYER_SHELL_EDGE_TOP
constexpr int kEdgeBottom = 3;        // GTK_LAYER_SHELL_EDGE_BOTTOM
constexpr int kKeyboardNone = 0;      // GTK_LAYER_SHELL_KEYBOARD_MODE_NONE
constexpr int kKeyboardOnDemand = 2;  // ..._KEYBOARD_MODE_ON_DEMAND (0.6+)

struct LayerShellApi {
  bool loaded = false;
  gboolean (*is_supported)() = nullptr;  // 0.6+
  void (*init_for_window)(GtkWindow*) = nullptr;
  gboolean (*is_layer_window)(GtkWindow*) = nullptr;
  void (*set_layer)(GtkWindow*, int) = nullptr;
  void (*set_anchor)(GtkWindow*, int, gboolean) = nullptr;
  void (*set_margin)(GtkWindow*, int, int) = nullptr;
  void (*set_keyboard_mode)(GtkWindow*, int) = nullptr;  // 0.6+
  void (*set_keyboard_interactivity)(GtkWindow*, gboolean) = nullptr;
  void (*set_exclusive_zone)(GtkWindow*, int) = nullptr;
  void (*set_namespace)(GtkWindow*, const char*) = nullptr;
  void (*set_monitor)(GtkWindow*, GdkMonitor*) = nullptr;
};

const LayerShellApi& LayerShell() {
  static LayerShellApi api = [] {
    LayerShellApi a;
    void* lib = dlopen("libgtk-layer-shell.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
      return a;
    auto sym = [lib](const char* name) { return dlsym(lib, name); };
    a.is_supported =
        reinterpret_cast<gboolean (*)()>(sym("gtk_layer_is_supported"));
    a.init_for_window = reinterpret_cast<void (*)(GtkWindow*)>(
        sym("gtk_layer_init_for_window"));
    a.is_layer_window = reinterpret_cast<gboolean (*)(GtkWindow*)>(
        sym("gtk_layer_is_layer_window"));
    a.set_layer =
        reinterpret_cast<void (*)(GtkWindow*, int)>(sym("gtk_layer_set_layer"));
    a.set_anchor = reinterpret_cast<void (*)(GtkWindow*, int, gboolean)>(
        sym("gtk_layer_set_anchor"));
    a.set_margin = reinterpret_cast<void (*)(GtkWindow*, int, int)>(
        sym("gtk_layer_set_margin"));
    a.set_keyboard_mode = reinterpret_cast<void (*)(GtkWindow*, int)>(
        sym("gtk_layer_set_keyboard_mode"));
    a.set_keyboard_interactivity =
        reinterpret_cast<void (*)(GtkWindow*, gboolean)>(
            sym("gtk_layer_set_keyboard_interactivity"));
    a.set_exclusive_zone = reinterpret_cast<void (*)(GtkWindow*, int)>(
        sym("gtk_layer_set_exclusive_zone"));
    a.set_namespace = reinterpret_cast<void (*)(GtkWindow*, const char*)>(
        sym("gtk_layer_set_namespace"));
    a.set_monitor = reinterpret_cast<void (*)(GtkWindow*, GdkMonitor*)>(
        sym("gtk_layer_set_monitor"));
    a.loaded = a.init_for_window && a.is_layer_window && a.set_layer &&
               a.set_anchor && a.set_margin && a.set_exclusive_zone;
    return a;
  }();
  return api;
}

#ifdef GDK_WINDOWING_WAYLAND
void RegistryGlobal(void* data, wl_registry*, uint32_t, const char* interface,
                    uint32_t) {
  if (strcmp(interface, "zwlr_layer_shell_v1") == 0)
    *static_cast<bool*>(data) = true;
}
void RegistryGlobalRemove(void*, wl_registry*, uint32_t) {}

// Whether the compositor advertises zwlr_layer_shell_v1, over a registry of
// GDK's own connection (a roundtrip on a private queue).
bool CompositorHasLayerShell(GdkDisplay* display) {
  wl_display* wl = gdk_wayland_display_get_wl_display(display);
  if (!wl)
    return false;
  wl_event_queue* queue = wl_display_create_queue(wl);
  if (!queue)
    return false;
  wl_display* wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(wl));
  wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
  wl_registry* registry = wl_display_get_registry(wrapper);
  bool found = false;
  static const wl_registry_listener listener = {RegistryGlobal,
                                                RegistryGlobalRemove};
  wl_registry_add_listener(registry, &listener, &found);
  wl_display_roundtrip_queue(wl, queue);
  wl_registry_destroy(registry);
  wl_proxy_wrapper_destroy(wrapper);
  wl_event_queue_destroy(queue);
  return found;
}
#endif

}  // namespace

bool LinuxIsWayland() {
#ifdef GDK_WINDOWING_WAYLAND
  GdkDisplay* display = gdk_display_get_default();
  return display && GDK_IS_WAYLAND_DISPLAY(display);
#else
  return false;
#endif
}

void LinuxSetClickPassthrough(uint64_t key, void* gtk_window, void* gdk_window,
                              bool enabled, double scale) {
  OverlayInputState& st = InputStates()[key];
  st.explicit_passthrough = enabled;
  // scale <= 0: the one the region was set with.
  ApplyInputShape(gtk_window, gdk_window, st, scale > 0 ? scale : st.scale);
}

bool LinuxIsClickPassthrough(uint64_t key) {
  auto it = InputStates().find(key);
  return it != InputStates().end() && it->second.explicit_passthrough;
}

void LinuxSetInputRegion(uint64_t key, void* gtk_window, void* gdk_window,
                         const InputRegion& region, double scale) {
  OverlayInputState& st = InputStates()[key];
  st.region = region;
  st.scale = scale > 0 ? scale : 1.0;
  ApplyInputShape(gtk_window, gdk_window, st, st.scale);
}

void LinuxForgetOverlayWindow(uint64_t key) {
  InputStates().erase(key);
}

bool LinuxSetSkipTaskbar(void* gtk_window, bool skip) {
  if (!gtk_window || LinuxIsWayland())
    return false;
  gtk_window_set_skip_taskbar_hint(GTK_WINDOW(gtk_window), skip);
  gtk_window_set_skip_pager_hint(GTK_WINDOW(gtk_window), skip);
  return true;
}

bool LinuxSetVisibleOnAllWorkspaces(void* gtk_window, bool visible) {
  if (!gtk_window || LinuxIsWayland())
    return false;
  if (visible)
    gtk_window_stick(GTK_WINDOW(gtk_window));
  else
    gtk_window_unstick(GTK_WINDOW(gtk_window));
  return true;
}

bool LinuxSetWindowLevel(void* gtk_window, int32_t level) {
  if (!gtk_window || !IsValidWindowLevel(level))
    return false;
  gtk_window_set_keep_above(GTK_WINDOW(gtk_window),
                            level != LAUFEY_WINDOW_LEVEL_NORMAL);
  return true;
}

bool LayerShellAvailable() {
  static int cached = -1;
  if (cached >= 0)
    return cached == 1;
  bool ok = false;
#ifdef GDK_WINDOWING_WAYLAND
  GdkDisplay* display = gdk_display_get_default();
  if (display && GDK_IS_WAYLAND_DISPLAY(display) && LayerShell().loaded) {
    if (LayerShell().is_supported)
      ok = LayerShell().is_supported();
    else
      ok = CompositorHasLayerShell(display);
  }
#endif
  cached = ok ? 1 : 0;
  return ok;
}

bool LayerShellInitOverlay(void* gtk_window, bool non_activating) {
  if (!gtk_window || !LayerShellAvailable())
    return false;
  const LayerShellApi& ls = LayerShell();
  GtkWindow* w = GTK_WINDOW(gtk_window);
  if (gtk_widget_get_realized(GTK_WIDGET(w)))
    return false;
  ls.init_for_window(w);
  if (!ls.is_layer_window(w))
    return false;
  ls.set_layer(w, kLayerOverlay);
  if (ls.set_namespace)
    ls.set_namespace(w, "laufey-overlay");
  // Anchored to the top-left corner: the margins are the position.
  ls.set_anchor(w, kEdgeLeft, TRUE);
  ls.set_anchor(w, kEdgeTop, TRUE);
  ls.set_margin(w, kEdgeLeft, 0);
  ls.set_margin(w, kEdgeTop, 0);
  ls.set_exclusive_zone(w, 0);
  if (ls.set_keyboard_mode)
    ls.set_keyboard_mode(w, non_activating ? kKeyboardNone : kKeyboardOnDemand);
  else if (ls.set_keyboard_interactivity)
    ls.set_keyboard_interactivity(w, FALSE);
  return true;
}

bool LayerShellIsLayerWindow(void* gtk_window) {
  if (!gtk_window || !LayerShell().loaded)
    return false;
  return LayerShell().is_layer_window(GTK_WINDOW(gtk_window));
}

namespace {

// The size the window is being given: gtk_window_get_size lags a resize by a
// configure round trip, and the default size is what LinuxResizeWindow set.
void LayerSurfaceSize(GtkWindow* w, int* width, int* height) {
  gtk_window_get_default_size(w, width, height);
  if (*width <= 0 || *height <= 0)
    gtk_window_get_size(w, width, height);
}

void PlaceLayerSurface(GtkWindow* w, int x, int y) {
  const LayerShellApi& ls = LayerShell();
  int width = 0, height = 0;
  LayerSurfaceSize(w, &width, &height);
  GdkDisplay* display = gtk_widget_get_display(GTK_WIDGET(w));
  GdkMonitor* monitor = display ? gdk_display_get_monitor_at_point(
                                      display, x + width / 2, y + height / 2)
                                : nullptr;
  if (!monitor && display)
    monitor = gdk_display_get_primary_monitor(display);
  if (!monitor) {
    // No output to measure from: the top-left of whichever the compositor
    // picks, as before.
    ls.set_anchor(w, kEdgeRight, FALSE);
    ls.set_anchor(w, kEdgeBottom, FALSE);
    ls.set_anchor(w, kEdgeLeft, TRUE);
    ls.set_anchor(w, kEdgeTop, TRUE);
    ls.set_margin(w, kEdgeLeft, x < 0 ? 0 : x);
    ls.set_margin(w, kEdgeTop, y < 0 ? 0 : y);
    return;
  }
  GdkRectangle g;
  gdk_monitor_get_geometry(monitor, &g);
  LayerEdgeAnchor a =
      ComputeLayerEdgeAnchor(g.x, g.y, g.width, g.height, x, y, width, height);
  // gtk-layer-shell remaps the surface to change its output: only when it did.
  if (ls.set_monitor &&
      g_object_get_data(G_OBJECT(w), "laufey-layer-monitor") != monitor) {
    ls.set_monitor(w, monitor);
    g_object_set_data(G_OBJECT(w), "laufey-layer-monitor", monitor);
  }
  ls.set_anchor(w, a.right ? kEdgeLeft : kEdgeRight, FALSE);
  ls.set_anchor(w, a.bottom ? kEdgeTop : kEdgeBottom, FALSE);
  ls.set_anchor(w, a.right ? kEdgeRight : kEdgeLeft, TRUE);
  ls.set_anchor(w, a.bottom ? kEdgeBottom : kEdgeTop, TRUE);
  ls.set_margin(w, a.right ? kEdgeLeft : kEdgeRight, 0);
  ls.set_margin(w, a.bottom ? kEdgeTop : kEdgeBottom, 0);
  ls.set_margin(w, a.right ? kEdgeRight : kEdgeLeft, a.margin_x);
  ls.set_margin(w, a.bottom ? kEdgeBottom : kEdgeTop, a.margin_y);
  // A surface commits with its next frame: an idle page has none coming, so
  // ask for one, or the new place would wait for the page to paint.
  if (gtk_widget_get_mapped(GTK_WIDGET(w)))
    gtk_widget_queue_draw(GTK_WIDGET(w));
  g_object_set_data(G_OBJECT(w), "laufey-layer-width", GINT_TO_POINTER(width));
  g_object_set_data(G_OBJECT(w), "laufey-layer-height",
                    GINT_TO_POINTER(height));
}

// A resize after the move: a margin from the far edge was computed for the old
// size, so the top-left corner the app asked for would drift. Place it again.
void OnLayerSurfaceAllocated(GtkWidget* widget, GdkRectangle*, gpointer) {
  GtkWindow* w = GTK_WINDOW(widget);
  GObject* o = G_OBJECT(w);
  int width = 0, height = 0;
  LayerSurfaceSize(w, &width, &height);
  if (GPOINTER_TO_INT(g_object_get_data(o, "laufey-layer-width")) == width &&
      GPOINTER_TO_INT(g_object_get_data(o, "laufey-layer-height")) == height) {
    return;
  }
  PlaceLayerSurface(w, GPOINTER_TO_INT(g_object_get_data(o, "laufey-layer-x")),
                    GPOINTER_TO_INT(g_object_get_data(o, "laufey-layer-y")));
}

}  // namespace

bool LayerShellMove(void* gtk_window, int x, int y) {
  if (!LayerShellIsLayerWindow(gtk_window))
    return false;
  GtkWindow* w = GTK_WINDOW(gtk_window);
  GObject* o = G_OBJECT(w);
  g_object_set_data(o, "laufey-layer-x", GINT_TO_POINTER(x));
  g_object_set_data(o, "laufey-layer-y", GINT_TO_POINTER(y));
  if (!g_object_get_data(o, "laufey-layer-moved")) {
    g_object_set_data(o, "laufey-layer-moved", GINT_TO_POINTER(1));
    g_signal_connect_after(w, "size-allocate",
                           G_CALLBACK(OnLayerSurfaceAllocated), nullptr);
  }
  PlaceLayerSurface(w, x, y);
  return true;
}

bool LayerShellPosition(void* gtk_window, int* x, int* y) {
  if (!LayerShellIsLayerWindow(gtk_window))
    return false;
  GObject* w = G_OBJECT(gtk_window);
  if (!g_object_get_data(w, "laufey-layer-moved"))
    return false;
  *x = GPOINTER_TO_INT(g_object_get_data(w, "laufey-layer-x"));
  *y = GPOINTER_TO_INT(g_object_get_data(w, "laufey-layer-y"));
  return true;
}

}  // namespace laufey_common
