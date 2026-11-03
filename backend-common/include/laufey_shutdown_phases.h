// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// How long each step of a host's shutdown took, from the app asking to quit
// (or the loop ending, when nothing asked) to the process ending. A quit that
// takes long enough for a user or a launcher to notice prints one line on
// stderr naming every step and its time:
//
//   laufey: the shutdown took 8026 ms (loop 40 ms, renderers 12 ms, ...)
//
// and with LAUFEY_TIMING=1 (laufey_timing.h) each step is also a timing line
// as it ends. No behaviour depends on it.

#ifndef LAUFEY_SHUTDOWN_PHASES_H_
#define LAUFEY_SHUTDOWN_PHASES_H_

#include <atomic>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

#include "laufey_timing.h"

namespace laufey_common {

// A shutdown that takes at least this long prints its steps.
constexpr long long kSlowShutdownMs = 2000;

// When the app first asked to quit (quit(), exit_app, the OS's quit), as
// steady_clock ticks since its epoch; 0 when it hasn't.
inline std::atomic<long long>& QuitRequestedTicks() {
  static std::atomic<long long> ticks{0};
  return ticks;
}

// Notes the first quit request; later ones keep the first time.
inline void NoteQuitRequested() {
  long long expected = 0;
  long long now = std::chrono::steady_clock::now().time_since_epoch().count();
  QuitRequestedTicks().compare_exchange_strong(expected, now ? now : 1);
}

class ShutdownPhases {
 public:
  using Clock = std::chrono::steady_clock;

  // Starts at `start`.
  explicit ShutdownPhases(Clock::time_point start) : last_(start) {}

  // Starts when the app asked to quit, else now (the loop ended on its own:
  // the last window closed).
  static ShutdownPhases FromQuitRequest(Clock::time_point now = Clock::now()) {
    long long ticks = QuitRequestedTicks().load();
    if (ticks == 0)
      return ShutdownPhases(now);
    Clock::time_point asked{Clock::duration(ticks)};
    return ShutdownPhases(asked < now ? asked : now);
  }

  // The step `name` ended at `now`: it took the time since the previous one.
  void Mark(const char* name, Clock::time_point now = Clock::now()) {
    long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_)
            .count();
    last_ = now;
    steps_.emplace_back(name, ms);
    total_ms_ += ms;
    if (TimingEnabled())
      Timing("shutdown " + std::string(name) + " " + std::to_string(ms) +
             " ms");
  }

  long long total_ms() const {
    return total_ms_;
  }
  const std::vector<std::pair<std::string, long long>>& steps() const {
    return steps_;
  }

  // "loop 40 ms, renderers 12 ms, ..." in the order the steps ended.
  std::string Summary() const {
    std::string out;
    for (const auto& step : steps_) {
      if (!out.empty())
        out += ", ";
      out += step.first + " " + std::to_string(step.second) + " ms";
    }
    return out;
  }

  // The stderr line when the steps so far took at least `slow_ms`, else "".
  std::string SlowLine(long long slow_ms = kSlowShutdownMs) const {
    if (total_ms_ < slow_ms)
      return "";
    return "laufey: the shutdown took " + std::to_string(total_ms_) + " ms (" +
           Summary() + ")";
  }

 private:
  Clock::time_point last_;
  std::vector<std::pair<std::string, long long>> steps_;
  long long total_ms_ = 0;
};

}  // namespace laufey_common

#endif  // LAUFEY_SHUTDOWN_PHASES_H_
