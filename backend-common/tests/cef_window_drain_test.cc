// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_cef_window_drain.h: the live-window table, the drain before
// CefShutdown (run by hand, and as steps inside a loop, with the renderer
// wait), against fake windows and a fake clock; and the creation gate
// the loop's end closes (laufey_window.h). No engine. Plain asserts, no
// framework.

#include "laufey_cef_window_drain.h"
#include "laufey_window.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

namespace {

// A window as the drain sees it: it goes `pumps_to_go` pumps after its close
// was asked, and only a firm (even_closing) ask moves one that ignores the
// first ask.
struct FakeWindow {
  bool closing = false;
  bool ignores_first_ask = false;
  bool never_goes = false;
  int pumps_to_go = 2;
  bool gone = false;
};

struct FakeHost {
  std::vector<FakeWindow> windows;
  Clock::time_point t{};
  int pumps = 0;
  std::vector<std::string> log;

  int Live() const {
    int n = 0;
    for (const FakeWindow& w : windows)
      n += w.gone ? 0 : 1;
    return n;
  }

  void Pump() {
    ++pumps;
    for (FakeWindow& w : windows) {
      if (w.gone || !w.closing || w.never_goes)
        continue;
      if (--w.pumps_to_go <= 0)
        w.gone = true;
    }
  }

  int Close(bool even_closing) {
    log.push_back(even_closing ? "close firm" : "close");
    int asked = 0;
    for (FakeWindow& w : windows) {
      if (w.gone || (!even_closing && w.closing))
        continue;
      ++asked;
      if (w.ignores_first_ask && !even_closing)
        continue;
      w.closing = true;
    }
    return asked;
  }

  WindowDrainResult Drain() {
    WindowDrainHooks hooks;
    hooks.live_windows = [this] { return Live(); };
    hooks.pump = [this] { Pump(); };
    hooks.close_live_windows = [this](bool firm) { return Close(firm); };
    hooks.now = [this] { return t; };
    hooks.sleep = [this](std::chrono::milliseconds d) { t += d; };
    return DrainWindowsBeforeShutdown(hooks);
  }
};

void TestTable() {
  LiveWindowTable<std::string> table;
  int a = 0, b = 0, c = 0;
  table.Add(&a, 1, "one");
  table.Add(&b, 0, "devtools");
  table.Add(&c, 3, "three");
  EXPECT(table.size() == 3);
  std::string found;
  EXPECT(table.Find(3, &found) && found == "three");
  EXPECT(table.Find(1, &found) && found == "one");
  // Id 0 (a window without a laufey id) never matches.
  EXPECT(!table.Find(0, &found));
  EXPECT(!table.Find(2, &found));
  auto all = table.All();
  EXPECT(all.size() == 3 && all[0].first == 1 && all[1].first == 0 &&
         all[2].first == 3);
  table.Remove(&a);
  EXPECT(table.size() == 2);
  EXPECT(!table.Find(1, &found));
  table.Remove(&a);  // Already gone: nothing.
  EXPECT(table.size() == 2);
  table.Remove(&b);
  table.Remove(&c);
  EXPECT(table.size() == 0);
}

void TestNothingLive() {
  FakeHost host;
  WindowDrainResult r = host.Drain();
  EXPECT(r.closed_at_start == 0 && r.closed_at_deadline == 0 && r.left == 0);
  EXPECT(host.log.empty());
  // The tail pumps run anyway, for the Browser deletions already posted.
  EXPECT(host.pumps == 5);
}

void TestClosingWindowsAreLeftToFinish() {
  // The usual quit: every window's close is under way when the loop ends.
  FakeHost host;
  host.windows.resize(2);
  for (FakeWindow& w : host.windows)
    w.closing = true;
  WindowDrainResult r = host.Drain();
  EXPECT(r.closed_at_start == 0);
  EXPECT(r.closed_at_deadline == 0);
  EXPECT(r.left == 0);
  EXPECT(host.log.size() == 1 && host.log[0] == "close");
}

void TestWindowOpenedDuringQuitIsClosed() {
  // A window app code opened after the loop ended (or whose creation was in
  // flight): nothing else closes it. The drain before this fix only waited
  // for it and returned with it live.
  FakeHost host;
  host.windows.resize(2);
  host.windows[0].closing = true;
  WindowDrainResult r = host.Drain();
  EXPECT(r.closed_at_start == 1);
  EXPECT(r.closed_at_deadline == 0);
  EXPECT(r.left == 0);
  // Well inside the 2 s budget.
  EXPECT(host.t < Clock::time_point{} + std::chrono::milliseconds(200));
}

void TestStuckWindowIsClosedAgainAtTheDeadline() {
  FakeHost host;
  host.windows.resize(1);
  host.windows[0].ignores_first_ask = true;
  WindowDrainResult r = host.Drain();
  EXPECT(r.closed_at_start == 1);
  EXPECT(r.closed_at_deadline == 1);
  EXPECT(r.left == 0);
  EXPECT(host.log.size() == 2 && host.log[1] == "close firm");
  EXPECT(host.t >= Clock::time_point{} + std::chrono::seconds(2));
}

void TestDrainIsBounded() {
  FakeHost host;
  host.windows.resize(1);
  host.windows[0].never_goes = true;
  WindowDrainResult r = host.Drain();
  EXPECT(r.closed_at_start == 1);
  EXPECT(r.closed_at_deadline == 1);
  EXPECT(r.left == 1);
  // 2 s, 1 s of grace, then the five tail pumps.
  EXPECT(host.t <= Clock::time_point{} + std::chrono::milliseconds(3100));
}

// The drain as steps (PreQuitDrain), as the Windows and Linux hosts run it:
// the loop runs between two steps (a pump here) and quits once a step
// answers true. `renderers` is how many renderer processes run, each ending
// `renderer_pumps` pumps after every window has gone; -1 leaves the hook
// unset.
struct StepHost : FakeHost {
  int renderers = -1;
  int renderer_pumps = 3;
  int steps = 0;

  WindowDrainResult Run() {
    PreQuitDrainHooks hooks;
    hooks.live_windows = [this] { return Live(); };
    hooks.close_live_windows = [this](bool firm) { return Close(firm); };
    if (renderers >= 0)
      hooks.renderers_running = [this] { return renderers; };
    PreQuitDrain drain(hooks);
    for (;;) {
      ++steps;
      if (drain.Step(t))
        break;
      bool windows_gone = Live() == 0;
      Pump();
      if (renderers > 0 && windows_gone && --renderer_pumps <= 0)
        renderers = 0;
      t += std::chrono::milliseconds(10);
    }
    // Answers true from then on.
    EXPECT(drain.Step(t));
    return drain.result();
  }
};

void TestStepsMatchTheDrainByHand() {
  // Same windows, same result and pumps as DrainWindowsBeforeShutdown.
  for (int kind = 0; kind < 4; ++kind) {
    FakeHost by_hand;
    StepHost steps;
    for (FakeHost* h :
         {static_cast<FakeHost*>(&by_hand), static_cast<FakeHost*>(&steps)}) {
      h->windows.resize(kind == 0 ? 0 : 2);
      if (kind >= 1)
        h->windows[0].closing = true;
      if (kind == 2)
        h->windows[1].ignores_first_ask = true;
      if (kind == 3)
        h->windows[1].never_goes = true;
    }
    WindowDrainResult a = by_hand.Drain();
    WindowDrainResult b = steps.Run();
    EXPECT(a.closed_at_start == b.closed_at_start);
    EXPECT(a.closed_at_deadline == b.closed_at_deadline);
    EXPECT(a.left == b.left);
    EXPECT(b.renderers_left == 0);
    EXPECT(by_hand.pumps == steps.pumps);
    EXPECT(by_hand.log == steps.log);
  }
}

void TestStepsWaitForRenderers() {
  // Windows go first, then the renderers; the loop quits only after both
  // (and the tail).
  StepHost host;
  host.windows.resize(1);
  host.windows[0].closing = true;
  host.renderers = 2;
  host.renderer_pumps = 4;
  WindowDrainResult r = host.Run();
  EXPECT(r.left == 0);
  EXPECT(r.renderers_left == 0);
  EXPECT(host.renderers == 0);
  // 2 pumps for the window, 4 for the renderers, 5 for the tail.
  EXPECT(host.pumps == 2 + 4 + 5);
}

void TestRendererWaitIsBounded() {
  StepHost host;
  host.renderers = 1;
  host.renderer_pumps = 1000000;
  WindowDrainResult r = host.Run();
  EXPECT(r.renderers_left == 1);
  // 2 s for the renderers, then the tail.
  EXPECT(host.t >= Clock::time_point{} + std::chrono::seconds(2));
  EXPECT(host.t <= Clock::time_point{} + std::chrono::milliseconds(2100));
}

void TestNoRendererHookNoWait() {
  StepHost host;
  WindowDrainResult r = host.Run();
  EXPECT(r.renderers_left == 0);
  EXPECT(host.pumps == 5);
}

void TestCreationGate() {
  // Before the loop ends, windows open (quit-on-last-window alone doesn't
  // refuse them: a tray app may open its first window later).
  EXPECT(!NewWindowsRefused());
  SetQuitOnLastWindowClosed(false);
  EXPECT(!NewWindowsRefused());
  SetQuitOnLastWindowClosed(true);
  EXPECT(!NewWindowsRefused());
  // The loop is ending: a window opened now would outlive it.
  MarkUiLoopEnding();
  EXPECT(NewWindowsRefused());
}

}  // namespace

int main() {
  TestTable();
  TestNothingLive();
  TestClosingWindowsAreLeftToFinish();
  TestWindowOpenedDuringQuitIsClosed();
  TestStuckWindowIsClosedAgainAtTheDeadline();
  TestDrainIsBounded();
  TestStepsMatchTheDrainByHand();
  TestStepsWaitForRenderers();
  TestRendererWaitIsBounded();
  TestNoRendererHookNoWait();
  TestCreationGate();
  std::printf("cef_window_drain_test: OK\n");
  return 0;
}
