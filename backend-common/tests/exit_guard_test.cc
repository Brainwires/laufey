// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// InstallUiExitGuard (laufey_io.h): a process that exits from another thread
// (the runtime's Deno.exit()) while the UI thread is busy must not keep
// running UI code while exit tears the libraries down. The UI thread here
// spins a GLib idle source; a second thread calls exit(); a handler that runs
// after the guard's checks that the UI thread has stopped.

#include <glib.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "laufey_io.h"
#include "laufey_ui_tasks.h"

namespace {

std::atomic<uint64_t> g_ticks{0};

// Registered before the guard, so it runs after it (atexit is LIFO).
void CheckParked() {
  uint64_t a = g_ticks.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  uint64_t b = g_ticks.load();
  if (a != b) {
    std::fprintf(stderr,
                 "laufey_exit_guard_test: the UI thread kept running during "
                 "exit (%llu -> %llu)\n",
                 static_cast<unsigned long long>(a),
                 static_cast<unsigned long long>(b));
    _exit(1);
  }
  std::printf("laufey_exit_guard_test: ok\n");
  std::fflush(stdout);
  _exit(0);
}

}  // namespace

int main() {
  laufey_common::UiTaskDispatcher::Get().Bind(
      [](void (*task)(void*), void* data) {
        struct Task {
          void (*task)(void*);
          void* data;
        };
        g_idle_add(
            [](gpointer p) -> gboolean {
              auto* t = static_cast<Task*>(p);
              t->task(t->data);
              delete t;
              return G_SOURCE_REMOVE;
            },
            new Task{task, data});
        return true;
      });
  atexit(CheckParked);
  laufey_common::InstallUiExitGuard();

  // A busy UI thread.
  g_idle_add(
      [](gpointer) -> gboolean {
        g_ticks++;
        return G_SOURCE_CONTINUE;
      },
      nullptr);
  std::thread([] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    exit(0);  // as Deno.exit() does, from the runtime thread
  }).detach();
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  g_main_loop_run(loop);
  return 2;  // not reached
}
