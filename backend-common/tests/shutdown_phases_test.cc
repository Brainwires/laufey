// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_shutdown_phases.h: the steps of a shutdown and their times, against
// a fake clock, and the stderr line a slow one prints. No engine. Plain
// asserts, no framework.

#include "laufey_shutdown_phases.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using namespace laufey_common;
using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

int main() {
  // Each step takes the time since the previous one.
  {
    Clock::time_point t0 = Clock::now();
    ShutdownPhases p(t0);
    p.Mark("loop", t0 + milliseconds(40));
    p.Mark("renderers", t0 + milliseconds(52));
    p.Mark("runtime", t0 + milliseconds(7052));
    EXPECT(p.steps().size() == 3);
    EXPECT(p.steps()[0].second == 40);
    EXPECT(p.steps()[1].second == 12);
    EXPECT(p.steps()[2].second == 7000);
    EXPECT(p.total_ms() == 7052);
    EXPECT(p.Summary() == "loop 40 ms, renderers 12 ms, runtime 7000 ms");
    EXPECT(p.SlowLine() ==
           "laufey: the shutdown took 7052 ms (loop 40 ms, renderers 12 ms, "
           "runtime 7000 ms)");
  }

  // A quick shutdown prints nothing; the bound itself is slow.
  {
    Clock::time_point t0 = Clock::now();
    ShutdownPhases p(t0);
    p.Mark("loop", t0 + milliseconds(300));
    p.Mark("CefShutdown", t0 + milliseconds(1999));
    EXPECT(p.SlowLine().empty());
    EXPECT(p.SlowLine(1999) != "");
    p.Mark("helpers", t0 + milliseconds(kSlowShutdownMs));
    EXPECT(p.total_ms() == kSlowShutdownMs);
    EXPECT(!p.SlowLine().empty());
  }

  // With no quit request the steps start now (the last window closed); the
  // first request starts them, and a later one keeps the first time.
  {
    EXPECT(QuitRequestedTicks().load() == 0);
    Clock::time_point now = Clock::now();
    ShutdownPhases fresh = ShutdownPhases::FromQuitRequest(now);
    fresh.Mark("loop", now + milliseconds(5));
    EXPECT(fresh.total_ms() == 5);

    NoteQuitRequested();
    long long first = QuitRequestedTicks().load();
    EXPECT(first != 0);
    NoteQuitRequested();
    EXPECT(QuitRequestedTicks().load() == first);

    Clock::time_point asked{Clock::duration(first)};
    ShutdownPhases p =
        ShutdownPhases::FromQuitRequest(asked + milliseconds(1000));
    p.Mark("loop", asked + milliseconds(1250));
    EXPECT(p.total_ms() == 1250);
  }

  std::printf("shutdown_phases_test: ok\n");
  return 0;
}
