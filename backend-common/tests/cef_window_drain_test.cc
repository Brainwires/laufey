// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_cef_window_drain.h: the live-window table and the drain before
// CefShutdown, against fake windows and a fake clock; and the creation gate
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
  TestCreationGate();
  std::printf("cef_window_drain_test: OK\n");
  return 0;
}
