// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// CEF: the app's live windows, and the drain that lets them go before
// CefShutdown.
//
// Chromium deletes a closed window's Browser in a task it posts when the
// window's close completes. One the ended loop never ran is deleted inside
// CefShutdown instead (BrowserManagerService::Shutdown, under ProfileManager's
// teardown), where Chromium 156 reads freed memory in ~TabDragServiceImpl
// (SIGSEGV, about 1 quit in 5,600 on macOS x64), and CEF requires every
// browser to be closed before it shuts down. So once the loop has ended the
// host pumps CEF until the windows are gone (DrainWindowsBeforeShutdown).
//
// The runtime thread still runs app code while the host drains: a window it
// opens then (a last-window handler that opens another), or one whose
// creation was in flight when the loop ended (a window without a Browser yet
// doesn't hold the loop open), would never be closed by anyone. So the drain
// asks every window still live when it starts to close, and asks again, more
// firmly, any still live when its budget runs out; and the host refuses new
// windows once the loop is ending (laufey_common::NewWindowsRefused, in
// laufey_window.h).
//
// LiveWindowTable also lets a call that arrives before the window's Browser
// is registered reach the window itself (show, size).
//
// Header-only; no CEF.

#ifndef LAUFEY_CEF_WINDOW_DRAIN_H_
#define LAUFEY_CEF_WINDOW_DRAIN_H_

#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

namespace laufey_common {

// The app's windows from creation to destruction, each under its owner (a
// key unique while it lives) and its laufey id (0 for one without). One
// thread (the CEF UI thread).
template <class Window>
class LiveWindowTable {
 public:
  void Add(const void* key, uint32_t laufey_id, Window window) {
    entries_.push_back(Entry{key, laufey_id, std::move(window)});
  }

  void Remove(const void* key) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
      if (it->key == key) {
        entries_.erase(it);
        return;
      }
    }
  }

  // The live window with laufey id `laufey_id`; laufey id 0 matches none.
  bool Find(uint32_t laufey_id, Window* out) const {
    if (laufey_id == 0)
      return false;
    for (const Entry& e : entries_) {
      if (e.laufey_id == laufey_id) {
        *out = e.window;
        return true;
      }
    }
    return false;
  }

  // Every live window with its laufey id, in creation order (a copy, so the
  // caller may close windows while it walks it).
  std::vector<std::pair<uint32_t, Window>> All() const {
    std::vector<std::pair<uint32_t, Window>> all;
    for (const Entry& e : entries_)
      all.emplace_back(e.laufey_id, e.window);
    return all;
  }

  int size() const {
    return static_cast<int>(entries_.size());
  }

 private:
  struct Entry {
    const void* key;
    uint32_t laufey_id;
    Window window;
  };
  std::vector<Entry> entries_;
};

struct WindowDrainHooks {
  // The windows created and not yet destroyed.
  std::function<int()> live_windows;
  // Runs CEF's queued UI-thread work once.
  std::function<void()> pump;
  // Asks every live window to close now, with no close-requested
  // negotiation. With `even_closing` false it skips one whose close is
  // already under way; true asks again. Returns how many it asked.
  std::function<int(bool even_closing)> close_live_windows;
  // The clock and the pause between pumps; real ones when unset.
  std::function<std::chrono::steady_clock::time_point()> now;
  std::function<void(std::chrono::milliseconds)> sleep;
};

struct WindowDrainResult {
  // Windows asked to close when the drain started (they were not closing),
  // and when its budget ran out (they were still live).
  int closed_at_start = 0;
  int closed_at_deadline = 0;
  // Windows still live when the drain returned.
  int left = 0;
};

// Before CefShutdown, once the loop has ended: asks any window not closing to
// close, pumps until every window has been destroyed (at most `budget`), asks
// any window still live to close again and pumps for at most `grace` more,
// then pumps a few more times for the tasks the last closes queued (the
// Browser's deletion is posted when its window's close completes).
inline WindowDrainResult DrainWindowsBeforeShutdown(
    const WindowDrainHooks& hooks,
    std::chrono::milliseconds budget = std::chrono::seconds(2),
    std::chrono::milliseconds grace = std::chrono::seconds(1)) {
  std::function<std::chrono::steady_clock::time_point()> now = hooks.now;
  if (!now)
    now = [] { return std::chrono::steady_clock::now(); };
  std::function<void(std::chrono::milliseconds)> sleep = hooks.sleep;
  if (!sleep)
    sleep = [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); };
  constexpr std::chrono::milliseconds kPause(10);
  auto pump_until_gone = [&](std::chrono::milliseconds limit) {
    auto deadline = now() + limit;
    while (hooks.live_windows() > 0 && now() < deadline) {
      hooks.pump();
      sleep(kPause);
    }
  };

  WindowDrainResult result;
  if (hooks.live_windows() > 0)
    result.closed_at_start = hooks.close_live_windows(false);
  pump_until_gone(budget);
  if (hooks.live_windows() > 0) {
    result.closed_at_deadline = hooks.close_live_windows(true);
    pump_until_gone(grace);
  }
  for (int i = 0; i < 5; ++i) {
    hooks.pump();
    sleep(kPause);
  }
  result.left = hooks.live_windows();
  return result;
}

}  // namespace laufey_common

#endif  // LAUFEY_CEF_WINDOW_DRAIN_H_
