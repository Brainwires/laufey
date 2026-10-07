// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// SIGTERM, SIGINT and SIGHUP end the app through its own quit, the path
// quit() and closing the last window take, on both Linux backends
// (laufey_backend_common.h). Without them the CEF host would take Chromium's
// handlers (chrome/browser/shutdown_signal_handlers_posix.cc), which for
// SIGTERM end the browser process at once with _exit(0), without the
// runtime's shutdown or CefShutdown; and the WebKitGTK host would be killed
// by the default action, without the runtime's shutdown or the web storage
// flush. The handlers are GLib sources on the default main context, which
// the UI thread runs, so the quit starts on that thread.

#include <glib-unix.h>
#include <glib.h>
#include <signal.h>

#include <cstring>
#include <iostream>

#include "laufey_backend_common.h"

namespace laufey_common {

namespace {

guint g_sources[3];
void (*g_quit)() = nullptr;

gboolean OnTerminationSignal(gpointer data) {
  std::cerr << "laufey: " << strsignal(GPOINTER_TO_INT(data)) << ", quitting"
            << std::endl;
  // This source ends by returning G_SOURCE_REMOVE; the others are removed.
  const guint self = g_source_get_id(g_main_current_source());
  for (guint& id : g_sources) {
    if (id == self)
      id = 0;
  }
  RemoveTerminationSignalHandlers();
  if (g_quit)
    g_quit();
  return G_SOURCE_REMOVE;
}

}  // namespace

void InstallTerminationSignalHandlers(void (*quit)()) {
  g_quit = quit;
  const int signals[] = {SIGTERM, SIGINT, SIGHUP};
  for (size_t i = 0; i < 3; i++) {
    if (g_sources[i] != 0)
      continue;
    g_sources[i] =
        g_unix_signal_add_full(G_PRIORITY_HIGH, signals[i], OnTerminationSignal,
                               GINT_TO_POINTER(signals[i]), nullptr);
  }
}

void RemoveTerminationSignalHandlers() {
  for (guint& id : g_sources) {
    if (id != 0) {
      g_source_remove(id);
      id = 0;
    }
  }
}

}  // namespace laufey_common
