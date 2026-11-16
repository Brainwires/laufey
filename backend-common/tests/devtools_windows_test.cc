// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_devtools_windows.h: which DevTools window an open brought up, from
// a baseline taken at the open, in the orders WebView2 produced on a real
// machine (a cold engine's first window, a close that was dropped, a second
// view of the same process). Plain asserts, no framework.

#include "laufey_devtools_windows.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using laufey_common::DevToolsOpenBaseline;
using laufey_common::DevToolsWindowSeen;
using laufey_common::IsNewDevToolsWindow;
using laufey_common::NewDevToolsWindow;

int main() {
  const uintptr_t kOld = 0x10, kNew = 0x20, kOther = 0x30;
  {
    // A window the engine makes after the open is new, hidden and untitled
    // as it is created; the windows already there are not.
    auto baseline = DevToolsOpenBaseline({{kOther, true}}, {});
    EXPECT(NewDevToolsWindow({{kOther, true}}, baseline, {}) == 0);
    EXPECT(NewDevToolsWindow({{kOther, true}, {kNew, false}}, baseline, {}) ==
           kNew);
  }
  {
    // The view's own window is closing (hidden, not destroyed yet) when the
    // next open comes, and the engine makes a new window for it (a cold
    // engine's first DevTools: the trace of brain's round 1/30). The closing
    // window stays in the baseline: it is never taken for the open, and a
    // close asked for at once waits for the new window instead.
    auto baseline = DevToolsOpenBaseline({{kOld, false}}, {kOld, kOld});
    EXPECT(baseline.size() == 1 && !baseline[0].visible);
    EXPECT(NewDevToolsWindow({{kOld, false}}, baseline, {}) == 0);
    EXPECT(NewDevToolsWindow({{kOld, false}, {kNew, false}}, baseline, {}) ==
           kNew);
    // Once the closing window is destroyed, the new one is still found.
    EXPECT(NewDevToolsWindow({{kNew, true}}, baseline, {}) == kNew);
  }
  {
    // The view's own window is shown (open, or a close that was dropped):
    // the engine brings it to the front, so it is the one to adopt.
    auto baseline = DevToolsOpenBaseline({{kOld, true}}, {kOld, 0});
    EXPECT(baseline.empty());
    EXPECT(NewDevToolsWindow({{kOld, true}}, baseline, {}) == kOld);
  }
  {
    // A window the baseline holds hidden that the engine shows again (the
    // view's window whose close was dropped while it was being set up).
    auto baseline = DevToolsOpenBaseline({{kOld, false}}, {});
    EXPECT(NewDevToolsWindow({{kOld, false}}, baseline, {}) == 0);
    EXPECT(IsNewDevToolsWindow({kOld, true}, baseline, {}));
    EXPECT(NewDevToolsWindow({{kOld, true}}, baseline, {}) == kOld);
  }
  {
    // Another view's window is never taken, shown or new.
    auto baseline = DevToolsOpenBaseline({}, {});
    EXPECT(NewDevToolsWindow({{kOther, false}}, baseline, {kOther}) == 0);
    EXPECT(NewDevToolsWindow({{kOther, true}, {kNew, false}}, baseline,
                             {kOther}) == kNew);
    auto hidden = DevToolsOpenBaseline({{kOther, false}}, {});
    EXPECT(!IsNewDevToolsWindow({kOther, true}, hidden, {kOther}));
  }
  {
    // A finder that serves an earlier open checks its result against the
    // current baseline: a window that baseline already holds, shown as it
    // was, is not new (the open was settled meanwhile).
    auto baseline = DevToolsOpenBaseline({{kOld, true}, {kNew, false}}, {});
    EXPECT(!IsNewDevToolsWindow({kOld, true}, baseline, {}));
    EXPECT(!IsNewDevToolsWindow({kNew, false}, baseline, {}));
    EXPECT(!IsNewDevToolsWindow({0, true}, baseline, {}));
  }
  std::printf("devtools_windows_test: ok\n");
  return 0;
}
