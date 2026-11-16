// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Which DevTools window an open_devtools brought up, where the engine gives
// no handle to it (WebView2: a top-level window of the browser process,
// shared by every view of that process). The backend takes a baseline of
// the process's DevTools windows when the open is asked for, and the
// window to adopt is the first one that is new against it:
//
// - a window not in the baseline (the engine made one), or
// - a window the baseline holds hidden that is shown now (the engine showed
//   the view's own window again: one whose close was dropped, or one it had
//   made and not shown yet).
//
// A window another view of the process has adopted or is closing is never
// new for this one. The view's open window is left out of the baseline when
// it is shown (the engine brings that one to the front instead of making
// another). A window the view is closing is never its own here: the backend
// lets an open start only once that window is destroyed, since an engine
// asked to open meanwhile makes a new window or brings the dying one back
// depending on how far the close has got. Header-only; handles are the
// platform's window handles as integers.

#ifndef LAUFEY_DEVTOOLS_WINDOWS_H_
#define LAUFEY_DEVTOOLS_WINDOWS_H_

#include <algorithm>
#include <cstdint>
#include <vector>

namespace laufey_common {

// One DevTools window of the browser process, and whether it is shown.
struct DevToolsWindowSeen {
  uintptr_t handle = 0;
  bool visible = false;
};

// The baseline for an open: the windows seen now, less the view's own
// (`own`: the window it has open) where shown.
inline std::vector<DevToolsWindowSeen> DevToolsOpenBaseline(
    const std::vector<DevToolsWindowSeen>& now,
    const std::vector<uintptr_t>& own) {
  std::vector<DevToolsWindowSeen> baseline;
  for (const DevToolsWindowSeen& w : now) {
    bool is_own =
        w.handle && std::find(own.begin(), own.end(), w.handle) != own.end();
    if (is_own && w.visible)
      continue;
    baseline.push_back(w);
  }
  return baseline;
}

// Whether `w` is new against `baseline`, for the view whose open took it
// (`others`: the windows other views of the process have adopted or are
// closing).
inline bool IsNewDevToolsWindow(const DevToolsWindowSeen& w,
                                const std::vector<DevToolsWindowSeen>& baseline,
                                const std::vector<uintptr_t>& others) {
  if (!w.handle ||
      std::find(others.begin(), others.end(), w.handle) != others.end())
    return false;
  auto in_baseline = std::find_if(
      baseline.begin(), baseline.end(),
      [&](const DevToolsWindowSeen& b) { return b.handle == w.handle; });
  if (in_baseline == baseline.end())
    return true;
  return !in_baseline->visible && w.visible;
}

// The first window of `now` that is new against `baseline`, or 0.
inline uintptr_t NewDevToolsWindow(
    const std::vector<DevToolsWindowSeen>& now,
    const std::vector<DevToolsWindowSeen>& baseline,
    const std::vector<uintptr_t>& others) {
  for (const DevToolsWindowSeen& w : now) {
    if (IsNewDevToolsWindow(w, baseline, others))
      return w.handle;
  }
  return 0;
}

}  // namespace laufey_common

#endif  // LAUFEY_DEVTOOLS_WINDOWS_H_
