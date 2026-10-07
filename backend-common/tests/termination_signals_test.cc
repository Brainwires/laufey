// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// InstallTerminationSignalHandlers (laufey_backend_common.h), the Linux
// hosts' SIGTERM / SIGINT / SIGHUP handling. For each signal, in a child
// process running the default main context:
//   - the first signal calls the quit callback once, on the thread running
//     the loop, and the process is not killed;
//   - it gives all three signals their default action back: a second one
//     (the same or another) ends the process with that signal;
// and RemoveTerminationSignalHandlers gives them back without a signal.

#include <glib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <thread>

#include "laufey_backend_common.h"

using namespace laufey_common;

namespace {

int g_failures = 0;

void Expect(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "laufey_termination_signals_test: FAIL %s\n", what);
    g_failures++;
  }
}

int g_quits = 0;
std::thread::id g_loop_thread;
bool g_quit_on_loop_thread = false;

void OnQuit() {
  g_quits++;
  g_quit_on_loop_thread = std::this_thread::get_id() == g_loop_thread;
}

// Child: handlers in, `first` raised while the loop runs; exits 10 + quits
// (with 20 added when the quit ran off the loop thread) once the loop has
// seen it, after raising `second` (0: none).
[[noreturn]] void Child(int first, int second) {
  g_loop_thread = std::this_thread::get_id();
  InstallTerminationSignalHandlers(OnQuit);
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  struct Ctx {
    GMainLoop* loop;
    int first;
  } ctx{loop, first};
  g_idle_add(
      [](gpointer p) -> gboolean {
        auto* c = static_cast<Ctx*>(p);
        kill(getpid(), c->first);
        return G_SOURCE_REMOVE;
      },
      &ctx);
  g_timeout_add(
      500,
      [](gpointer p) -> gboolean {
        g_main_loop_quit(static_cast<GMainLoop*>(p));
        return G_SOURCE_REMOVE;
      },
      loop);
  g_main_loop_run(loop);
  if (second != 0) {
    kill(getpid(), second);
    // Delivery to this thread is synchronous; a handled signal falls through.
    usleep(200 * 1000);
  }
  _exit(10 + g_quits + (g_quit_on_loop_thread ? 0 : 20));
}

int Run(int first, int second) {
  std::fflush(stderr);
  pid_t pid = fork();
  if (pid == 0)
    Child(first, second);
  int status = 0;
  waitpid(pid, &status, 0);
  if (WIFSIGNALED(status))
    return -WTERMSIG(status);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1000;
}

}  // namespace

int main() {
  const int signals[] = {SIGTERM, SIGINT, SIGHUP};
  for (int sig : signals) {
    // Handled once, on the loop's thread; the process lives on.
    Expect(Run(sig, 0) == 11, "the first signal quits once on the UI thread");
    // The second one takes the default action.
    Expect(Run(sig, sig) == -sig, "a second signal takes its default action");
  }
  // After one of them, the others are back to their default action too.
  Expect(Run(SIGTERM, SIGINT) == -SIGINT, "SIGTERM gives SIGINT back");
  Expect(Run(SIGHUP, SIGTERM) == -SIGTERM, "SIGHUP gives SIGTERM back");

  // Removed without a signal: the default action.
  {
    std::fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
      InstallTerminationSignalHandlers(OnQuit);
      RemoveTerminationSignalHandlers();
      kill(getpid(), SIGTERM);
      usleep(200 * 1000);
      _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    Expect(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
           "removed handlers give SIGTERM its default action");
  }

  if (g_failures) {
    std::fprintf(stderr, "laufey_termination_signals_test: %d failure(s)\n",
                 g_failures);
    return 1;
  }
  std::printf("laufey_termination_signals_test: ok\n");
  return 0;
}
