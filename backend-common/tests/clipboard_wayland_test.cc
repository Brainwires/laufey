// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The Linux clipboard through ext-data-control-v1 against the session's real
// compositor (clipboard_data_control_linux.h): text, HTML with its text
// alternative, a PNG larger than a pipe's buffer (our own source answers our
// own read on the same connection), the formats list, and change events. No
// window is shown, so nothing here has keyboard focus: the core Wayland
// clipboard would refuse every one of these. Exits 77 (skipped) outside a
// Wayland session or where the compositor has no data-control (GNOME, CI).

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "../src/clipboard_data_control_linux.h"
#include "laufey_backend_common.h"
#include "laufey_io.h"

using namespace laufey_common;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

namespace {

std::atomic<int> g_changes{0};

void OnChange(void*) {
  g_changes++;
}

template <typename F>
bool WaitFor(F cond) {
  for (int i = 0; i < 300; i++) {
    if (cond())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return cond();
}

// A noise image: its PNG is far larger than a pipe's 64 KiB buffer.
std::string NoisePng() {
  GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, 400, 400);
  guchar* px = gdk_pixbuf_get_pixels(pixbuf);
  int stride = gdk_pixbuf_get_rowstride(pixbuf);
  uint32_t x = 2463534242u;
  for (int y = 0; y < 400; y++) {
    for (int i = 0; i < 400 * 3; i++) {
      x ^= x << 13;
      x ^= x >> 17;
      x ^= x << 5;
      px[y * stride + i] = static_cast<guchar>(x);
    }
  }
  gchar* buf = nullptr;
  gsize len = 0;
  EXPECT(gdk_pixbuf_save_to_buffer(pixbuf, &buf, &len, "png", nullptr,
                                   nullptr));
  std::string png(buf, len);
  g_free(buf);
  g_object_unref(pixbuf);
  return png;
}

std::string Take(char* s) {
  std::string out = s ? s : "";
  free(s);
  return out;
}

}  // namespace

int main() {
  const char* wayland = getenv("WAYLAND_DISPLAY");
  if (!wayland || !*wayland || !data_control::Available()) {
    std::printf(
        "laufey_clipboard_wayland_test: no ext-data-control here, skipped\n");
    return 77;
  }
  // Change events are delivered "on the GTK thread"; here, inline.
  SetGtkThread([](std::function<void()> fn) { fn(); }, [] { return true; });

  // Text.
  ClipboardWriteTextLinux("laufey data-control ✓");
  EXPECT(Take(ClipboardReadTextLinux()) == "laufey data-control ✓");
  std::string formats = Take(ClipboardReadFormatsLinux());
  EXPECT(formats == "text/plain");

  // HTML with a text alternative.
  EXPECT(ClipboardWriteHtmlLinux("<b>bold</b>", "bold"));
  EXPECT(Take(ClipboardReadHtmlLinux()) == "<b>bold</b>");
  EXPECT(Take(ClipboardReadTextLinux()) == "bold");
  formats = Take(ClipboardReadFormatsLinux());
  EXPECT(formats.find("text/html") != std::string::npos);
  EXPECT(formats.find("text/plain") != std::string::npos);

  // A PNG bigger than a pipe buffer, verbatim.
  std::string png = NoisePng();
  EXPECT(png.size() > 256 * 1024);
  EXPECT(ClipboardWriteImageLinux(
      reinterpret_cast<const uint8_t*>(png.data()), png.size()));
  size_t len = 0;
  uint8_t* got = ClipboardReadImageLinux(&len);
  EXPECT(got && len == png.size() && memcmp(got, png.data(), len) == 0);
  free(got);
  EXPECT(Take(ClipboardReadFormatsLinux()) == "image/png");

  // Change events, this process's own writes included.
  SetClipboardChangeHandler(OnChange, nullptr);
  int before = g_changes.load();
  ClipboardWriteTextLinux("changed");
  EXPECT(WaitFor([&] { return g_changes.load() > before; }));
  SetClipboardChangeHandler(nullptr, nullptr);

  std::printf("laufey_clipboard_wayland_test: ok\n");
  std::fflush(stdout);
  // The data-control thread is detached; skip static teardown.
  std::_Exit(0);
}
