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
// browser to be closed before it shuts down. So the host lets CEF run until
// the windows are gone before CefShutdown.
//
// Where the loop is CefRunMessageLoop (Windows, Linux) that has to happen
// before the loop ends: once CefQuitMessageLoop has let the loop end,
// Chromium's shutdown has begun (BrowserProcessImpl::Unpin marks the
// KeepAliveRegistry shutting down), and CEF 156.0.28's first
// CefDoMessageLoopWork registers a keep-alive (CEF issue 4353), which
// CHECK-fails then; running the loop again takes one too. So those hosts run
// the drain as steps inside the loop and quit it at the end (PreQuitDrain).
// macOS pumps CEF itself from [NSApp run] (external_message_pump): that
// keep-alive is held from the first pump at launch until CefShutdown and the
// loop's end doesn't touch it, so the host pumps by hand after the loop
// (DrainWindowsBeforeShutdown).
//
// The runtime thread still runs app code while the host drains: a window it
// opens then (a last-window handler that opens another), or one whose
// creation was in flight when the quit began (a window without a Browser yet
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

struct WindowDrainResult {
  // Windows asked to close when the drain started (they were not closing),
  // and when its budget ran out (they were still live).
  int closed_at_start = 0;
  int closed_at_deadline = 0;
  // Windows still live when the drain returned.
  int left = 0;
  // Renderer processes still running when the drain returned (0 when it
  // didn't wait for them).
  int renderers_left = 0;
};

struct PreQuitDrainHooks {
  // The windows created and not yet destroyed.
  std::function<int()> live_windows;
  // Asks every live window to close now, with no close-requested
  // negotiation. With `even_closing` false it skips one whose close is
  // already under way; true asks again. Returns how many it asked.
  std::function<int(bool even_closing)> close_live_windows;
  // The renderer processes still running; unset: the drain doesn't wait
  // for them.
  std::function<int()> renderers_running;
};

// The drain as steps, for a host that runs it inside its loop: the loop runs
// CEF's work between two steps, so nothing pumps CEF by hand. Step(now)
// answers true once the loop may end:
//   1. asks any window not closing to close, and waits until every window
//      has been destroyed (at most `budget`); then asks any window still
//      live to close again and waits at most `grace` more;
//   2. waits until the renderer processes have ended (at most
//      `renderer_budget`; only with renderers_running);
//   3. lets the loop run `tail_steps` more times, for the tasks the last
//      closes queued (the Browser's deletion is posted when its window's
//      close completes).
class PreQuitDrain {
 public:
  explicit PreQuitDrain(
      PreQuitDrainHooks hooks,
      std::chrono::milliseconds budget = std::chrono::seconds(2),
      std::chrono::milliseconds grace = std::chrono::seconds(1),
      std::chrono::milliseconds renderer_budget = std::chrono::seconds(2),
      int tail_steps = 5)
      : hooks_(std::move(hooks)),
        budget_(budget),
        grace_(grace),
        renderer_budget_(renderer_budget),
        tail_left_(tail_steps) {}

  // True when done; call again after the loop has run a while otherwise.
  // Once it has answered true it keeps answering true.
  bool Step(std::chrono::steady_clock::time_point now) {
    for (;;) {
      switch (phase_) {
        case Phase::kStart:
          if (hooks_.live_windows() > 0)
            result_.closed_at_start = hooks_.close_live_windows(false);
          deadline_ = now + budget_;
          phase_ = Phase::kWindows;
          continue;
        case Phase::kWindows:
          if (hooks_.live_windows() > 0 && now < deadline_)
            return false;
          if (hooks_.live_windows() > 0) {
            result_.closed_at_deadline = hooks_.close_live_windows(true);
            deadline_ = now + grace_;
            phase_ = Phase::kWindowsGrace;
          } else {
            StartRenderers(now);
          }
          continue;
        case Phase::kWindowsGrace:
          if (hooks_.live_windows() > 0 && now < deadline_)
            return false;
          StartRenderers(now);
          continue;
        case Phase::kRenderers:
          if (hooks_.renderers_running() > 0 && now < deadline_)
            return false;
          phase_ = Phase::kTail;
          continue;
        case Phase::kTail:
          if (tail_left_ > 0) {
            --tail_left_;
            return false;
          }
          result_.left = hooks_.live_windows();
          if (hooks_.renderers_running)
            result_.renderers_left = hooks_.renderers_running();
          phase_ = Phase::kDone;
          return true;
        case Phase::kDone:
          return true;
      }
    }
  }

  // Meaningful once Step answered true.
  const WindowDrainResult& result() const {
    return result_;
  }

 private:
  enum class Phase {
    kStart,
    kWindows,
    kWindowsGrace,
    kRenderers,
    kTail,
    kDone
  };

  void StartRenderers(std::chrono::steady_clock::time_point now) {
    if (hooks_.renderers_running) {
      deadline_ = now + renderer_budget_;
      phase_ = Phase::kRenderers;
    } else {
      phase_ = Phase::kTail;
    }
  }

  PreQuitDrainHooks hooks_;
  std::chrono::milliseconds budget_;
  std::chrono::milliseconds grace_;
  std::chrono::milliseconds renderer_budget_;
  int tail_left_;
  Phase phase_ = Phase::kStart;
  std::chrono::steady_clock::time_point deadline_{};
  WindowDrainResult result_;
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

// PreQuitDrain's window steps run by hand, for a host whose loop has ended
// but may still be pumped (macOS: the external message pump holds CEF's
// keep-alive until CefShutdown): `pump` with a 10 ms pause between steps.
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
  PreQuitDrainHooks steps;
  steps.live_windows = hooks.live_windows;
  steps.close_live_windows = hooks.close_live_windows;
  PreQuitDrain drain(steps, budget, grace);
  while (!drain.Step(now())) {
    hooks.pump();
    sleep(kPause);
  }
  return drain.result();
}

}  // namespace laufey_common

#endif  // LAUFEY_CEF_WINDOW_DRAIN_H_
