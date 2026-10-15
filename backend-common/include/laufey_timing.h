// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Timing lines for diagnosing startup and streaming stalls, on stderr only
// when LAUFEY_TIMING=1 (native-e2e sets it); otherwise nothing is computed
// or printed. Each line:
//
//   laufey-timing <UTC wall clock, ms> +<ms since the first line> <event> ...
//
// The wall clock lines up with Chromium's net log (its timeTickOffset) and
// the CI log's timestamps. No behaviour depends on it.

#ifndef LAUFEY_TIMING_H_
#define LAUFEY_TIMING_H_

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>

namespace laufey_common {

inline bool TimingEnabled() {
  static const bool on = [] {
    const char* v = std::getenv("LAUFEY_TIMING");
    return v && std::string(v) == "1";
  }();
  return on;
}

inline void Timing(const std::string& event) {
  if (!TimingEnabled())
    return;
  static std::mutex mutex;
  static const auto start = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex);
  auto now = std::chrono::system_clock::now();
  std::time_t secs = std::chrono::system_clock::to_time_t(now);
  long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     now.time_since_epoch())
                     .count() %
                 1000;
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &secs);
#else
  gmtime_r(&secs, &tm);
#endif
  char wall[32];
  std::strftime(wall, sizeof(wall), "%H:%M:%S", &tm);
  long long since = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
  std::fprintf(stderr, "laufey-timing %s.%03lld +%lld %s\n", wall, ms, since,
               event.c_str());
  std::fflush(stderr);
}

}  // namespace laufey_common

#endif  // LAUFEY_TIMING_H_
