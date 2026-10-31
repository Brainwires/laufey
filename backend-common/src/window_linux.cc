// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// GTK3 window size and decorations for the WebView backend; see
// laufey_window.h.

#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif

#include "laufey_overlay.h"
#include "laufey_window.h"

namespace laufey_common {

namespace {

// LinuxNewContentBox's type: a GtkBox whose natural size is its minimum.
struct LaufeyContentBox {
  GtkBox parent;
};
struct LaufeyContentBoxClass {
  GtkBoxClass parent_class;
};

G_DEFINE_TYPE(LaufeyContentBox, laufey_content_box, GTK_TYPE_BOX)

GtkWidgetClass* ContentBoxParent() {
  return GTK_WIDGET_CLASS(laufey_content_box_parent_class);
}

void ContentBoxWidth(GtkWidget* w, gint* min, gint* nat) {
  ContentBoxParent()->get_preferred_width(w, min, nat);
  *nat = *min;
}

void ContentBoxHeight(GtkWidget* w, gint* min, gint* nat) {
  ContentBoxParent()->get_preferred_height(w, min, nat);
  *nat = *min;
}

void ContentBoxWidthForHeight(GtkWidget* w, gint height, gint* min, gint* nat) {
  ContentBoxParent()->get_preferred_width_for_height(w, height, min, nat);
  *nat = *min;
}

void ContentBoxHeightForWidth(GtkWidget* w, gint width, gint* min, gint* nat) {
  ContentBoxParent()->get_preferred_height_for_width(w, width, min, nat);
  *nat = *min;
}

void laufey_content_box_class_init(LaufeyContentBoxClass* klass) {
  GtkWidgetClass* widget = GTK_WIDGET_CLASS(klass);
  widget->get_preferred_width = ContentBoxWidth;
  widget->get_preferred_height = ContentBoxHeight;
  widget->get_preferred_width_for_height = ContentBoxWidthForHeight;
  widget->get_preferred_height_for_width = ContentBoxHeightForWidth;
}

void laufey_content_box_init(LaufeyContentBox*) {}

void OnFramelessWindowRealize(GtkWidget* widget, gpointer) {
#ifdef GDK_WINDOWING_WAYLAND
  GdkWindow* gw = gtk_widget_get_window(widget);
  if (gw && GDK_IS_WAYLAND_WINDOW(gw) && !LayerShellIsLayerWindow(widget))
    gdk_wayland_window_announce_csd(gw);
#else
  (void)widget;
#endif
}

}  // namespace

std::string LinuxGtkModulesForDisplay(const char* gtk_modules,
                                      const char* wayland_display,
                                      const char* gdk_backend) {
  std::string modules = gtk_modules ? gtk_modules : "";
  if (!wayland_display || !*wayland_display)
    return modules;
  // GDK_BACKEND is a comma list in priority order: the first entry decides.
  if (gdk_backend && *gdk_backend) {
    std::string first(gdk_backend);
    first = first.substr(0, first.find(','));
    if (first != "wayland" && first != "*")
      return modules;
  }
  std::string out;
  size_t start = 0;
  while (start <= modules.size()) {
    size_t end = modules.find_first_of(":,", start);
    if (end == std::string::npos)
      end = modules.size();
    std::string name = modules.substr(start, end - start);
    if (!name.empty() && name != "appmenu-gtk-module") {
      if (!out.empty())
        out += ':';
      out += name;
    }
    start = end + 1;
  }
  return out;
}

void* LinuxNewContentBox() {
  return g_object_new(laufey_content_box_get_type(), "orientation",
                      GTK_ORIENTATION_VERTICAL, "spacing", 0, nullptr);
}

void LinuxResizeWindow(void* gtk_window, int width, int height) {
  if (!gtk_window || width <= 0 || height <= 0)
    return;
  GtkWindow* w = GTK_WINDOW(gtk_window);
  gtk_window_set_default_size(w, width, height);
  gtk_window_resize(w, width, height);
}

void LinuxSetResizable(void* gtk_window, bool resizable) {
  if (!gtk_window)
    return;
  GtkWindow* w = GTK_WINDOW(gtk_window);
  if (!resizable && gtk_window_get_resizable(w)) {
    int width = 0, height = 0;
    gtk_window_get_size(w, &width, &height);
    if (width > 0 && height > 0)
      gtk_window_set_default_size(w, width, height);
  }
  gtk_window_set_resizable(w, resizable);
}

void LinuxWatchFramelessDecorations(void* gtk_window) {
  if (!gtk_window)
    return;
  g_signal_connect_after(G_OBJECT(gtk_window), "realize",
                         G_CALLBACK(OnFramelessWindowRealize), nullptr);
}

}  // namespace laufey_common
