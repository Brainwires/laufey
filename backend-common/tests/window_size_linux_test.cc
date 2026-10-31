// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// GTK3 window sizes (laufey_window.h, LinuxResizeWindow / LinuxSetResizable),
// on a private Xvfb, for a window created at the backend's 800x600 default:
//   - resized to 260x300, then made non-resizable, it stays 260x300 (it
//     snapped back to 800x600: GTK3 holds a non-resizable window at its
//     default size);
//   - a non-resizable window still takes a new size (300x340), and a smaller
//     one again (240x220);
//   - made resizable again, it keeps its size and takes the next one;
//   - a page taller than a non-resizable window doesn't grow it
//     (LinuxNewContentBox; it grew to the page's height).
// And GTK_MODULES on a Wayland display (LinuxGtkModulesForDisplay), no display
// needed. Exits 77 (skipped) without Xvfb, after those.

#include <fcntl.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "laufey_window.h"

using namespace laufey_common;

namespace {

pid_t g_xvfb = -1;

[[noreturn]] void Finish(int code) {
  if (g_xvfb > 0)
    kill(g_xvfb, SIGTERM);
  std::fflush(stdout);
  std::_Exit(code);
}

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      Finish(1);                                                             \
    }                                                                        \
  } while (0)

// Starts a private Xvfb and points DISPLAY at it; its pid, or -1.
pid_t StartXvfb() {
  gchar* xvfb = g_find_program_in_path("Xvfb");
  if (!xvfb)
    return -1;
  int fds[2];
  EXPECT(pipe(fds) == 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    int null = open("/dev/null", O_RDWR);
    dup2(null, STDIN_FILENO);
    dup2(null, STDOUT_FILENO);
    dup2(null, STDERR_FILENO);
    std::string fd = std::to_string(fds[1]);
    execl(xvfb, xvfb, "-displayfd", fd.c_str(), "-nolisten", "tcp", "-screen",
          "0", "1280x1024x24", static_cast<char*>(nullptr));
    _exit(127);
  }
  g_free(xvfb);
  close(fds[1]);
  pollfd p = {fds[0], POLLIN, 0};
  char buf[32] = {};
  if (poll(&p, 1, 10000) != 1 || read(fds[0], buf, sizeof(buf) - 1) <= 0) {
    kill(pid, SIGKILL);
    return -1;
  }
  close(fds[0]);
  std::string display = std::string(":") + strtok(buf, "\n");
  setenv("DISPLAY", display.c_str(), 1);
  return pid;
}

// Runs the loop until the window's allocation is width x height (the X
// server has applied the configure), or 3 s pass; the last size seen.
void WaitForSize(GtkWidget* window, int width, int height, int* out_w,
                 int* out_h) {
  gint64 end = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
  int w = 0, h = 0;
  do {
    while (gtk_events_pending())
      gtk_main_iteration_do(FALSE);
    w = gtk_widget_get_allocated_width(window);
    h = gtk_widget_get_allocated_height(window);
    if (w == width && h == height)
      break;
    g_usleep(10 * 1000);
  } while (g_get_monotonic_time() < end);
  *out_w = w;
  *out_h = h;
}

void ExpectSize(GtkWidget* window, int width, int height) {
  int w = 0, h = 0;
  WaitForSize(window, width, height, &w, &h);
  if (w != width || h != height) {
    std::fprintf(stderr, "expected %dx%d, the window is %dx%d\n", width, height,
                 w, h);
    Finish(1);
  }
}

// A child like a web view with a page taller than the window: no minimum
// size, a natural height of `natural` (WebKitWebView's natural height is its
// page's content height).
GtkWidget* TallPage(int natural) {
  GtkWidget* scroller = gtk_scrolled_window_new(nullptr, nullptr);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(
      GTK_SCROLLED_WINDOW(scroller), TRUE);
  GtkWidget* content = gtk_drawing_area_new();
  gtk_widget_set_size_request(content, 10, natural);
  gtk_container_add(GTK_CONTAINER(scroller), content);
  return scroller;
}

// A top-level as the backend makes one: undecorated, created at the 800x600
// default, its content in LinuxNewContentBox (or a plain box), holding a page
// `natural` pixels tall (0: a child that asks for no size).
GtkWidget* NewWindow(int natural = 0, bool content_box = true) {
  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_decorated(GTK_WINDOW(window), FALSE);
  gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
  GtkWidget* box = content_box ? static_cast<GtkWidget*>(LinuxNewContentBox())
                               : gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_container_add(GTK_CONTAINER(window), box);
  GtkWidget* page = natural > 0 ? TallPage(natural) : gtk_drawing_area_new();
  gtk_box_pack_start(GTK_BOX(box), page, TRUE, TRUE, 0);
  return window;
}

}  // namespace

// GTK_MODULES without appmenu-gtk-module on a Wayland display, else as is.
void TestGtkModulesForDisplay() {
  const char* ubuntu = "gail:atk-bridge:appmenu-gtk-module";
  EXPECT(LinuxGtkModulesForDisplay(ubuntu, "wayland-0", nullptr) ==
         "gail:atk-bridge");
  EXPECT(LinuxGtkModulesForDisplay(ubuntu, "wayland-0", "wayland,x11") ==
         "gail:atk-bridge");
  EXPECT(LinuxGtkModulesForDisplay(ubuntu, "wayland-0", "*") ==
         "gail:atk-bridge");
  EXPECT(LinuxGtkModulesForDisplay("appmenu-gtk-module", "wayland-0", "") ==
         "");
  EXPECT(LinuxGtkModulesForDisplay("a,appmenu-gtk-module,b", "wayland-1",
                                   nullptr) == "a:b");
  // An X11 session (no Wayland display, or GDK_BACKEND=x11): unchanged.
  EXPECT(LinuxGtkModulesForDisplay(ubuntu, nullptr, nullptr) == ubuntu);
  EXPECT(LinuxGtkModulesForDisplay(ubuntu, "", nullptr) == ubuntu);
  EXPECT(LinuxGtkModulesForDisplay(ubuntu, "wayland-0", "x11") == ubuntu);
  EXPECT(LinuxGtkModulesForDisplay(ubuntu, "wayland-0", "x11,wayland") ==
         ubuntu);
  EXPECT(LinuxGtkModulesForDisplay(nullptr, "wayland-0", nullptr) == "");
  EXPECT(LinuxGtkModulesForDisplay("gail", "wayland-0", nullptr) == "gail");
}

int main() {
  TestGtkModulesForDisplay();
  g_xvfb = StartXvfb();
  if (g_xvfb < 0) {
    std::printf("window_size_linux_test: skipped (no Xvfb)\n");
    return 77;
  }
  setenv("GDK_BACKEND", "x11", 1);
  EXPECT(gtk_init_check(nullptr, nullptr));

  // As the backend creates every window: the 800x600 default, then the
  // size the embedder asks for.
  GtkWidget* window = NewWindow();
  LinuxResizeWindow(window, 260, 300);
  gtk_widget_show_all(window);
  ExpectSize(window, 260, 300);

  LinuxSetResizable(window, false);
  EXPECT(!gtk_window_get_resizable(GTK_WINDOW(window)));
  ExpectSize(window, 260, 300);

  LinuxResizeWindow(window, 300, 340);
  ExpectSize(window, 300, 340);
  // Smaller again (not under GTK's 200x200 natural size of a top-level,
  // which a non-resizable one never goes below).
  LinuxResizeWindow(window, 240, 220);
  ExpectSize(window, 240, 220);

  LinuxSetResizable(window, true);
  EXPECT(gtk_window_get_resizable(GTK_WINDOW(window)));
  ExpectSize(window, 240, 220);
  LinuxResizeWindow(window, 420, 360);
  ExpectSize(window, 420, 360);

  // Created non-resizable (as a tray panel is) before it is first shown.
  GtkWidget* panel = NewWindow();
  LinuxResizeWindow(panel, 360, 480);
  LinuxSetResizable(panel, false);
  gtk_widget_show_all(panel);
  ExpectSize(panel, 360, 480);

  // A page taller than the window doesn't grow it (it did, to the page's
  // height, in a plain GtkBox: GTK3 sizes a non-resizable window to its
  // natural size).
  GtkWidget* tall = NewWindow(900);
  LinuxResizeWindow(tall, 360, 480);
  LinuxSetResizable(tall, false);
  gtk_widget_show_all(tall);
  ExpectSize(tall, 360, 480);
  int min_h = 0, nat_h = 0;
  gtk_widget_get_preferred_height(gtk_bin_get_child(GTK_BIN(tall)), &min_h,
                                  &nat_h);
  EXPECT(nat_h == min_h);
  gtk_widget_destroy(tall);
  // The same window in a plain box does grow: what the content box is for.
  GtkWidget* plain = NewWindow(900, false);
  LinuxResizeWindow(plain, 360, 480);
  LinuxSetResizable(plain, false);
  gtk_widget_show_all(plain);
  int w = 0, h = 0;
  WaitForSize(plain, 360, 900, &w, &h);
  EXPECT(h > 480);
  gtk_widget_destroy(plain);

  // Nothing for a NULL window or a size that isn't one.
  LinuxResizeWindow(nullptr, 10, 10);
  LinuxSetResizable(nullptr, false);
  LinuxWatchFramelessDecorations(nullptr);
  LinuxResizeWindow(panel, 0, -1);
  ExpectSize(panel, 360, 480);

  gtk_widget_destroy(panel);
  gtk_widget_destroy(window);
  std::printf("window_size_linux_test: all passed\n");
  Finish(0);
}
