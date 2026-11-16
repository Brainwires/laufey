// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// When a WebView2 backend retries a window's failed controller creation
// (from a new environment, as WebView2's documentation asks). A creation
// fails while the user data folder is still held by a browser process that
// is shutting down (ERROR_BUSY, E_FAIL: an earlier instance of the app, or
// one that was killed), which on a starved machine takes seconds, so the
// retries back off (250 ms, doubling, at most 2 s apart) for a budget of
// time rather than a fixed count; ERROR_INVALID_STATE (a running instance
// with other environment options) is never retried. The budget ends well
// before the window's web view ready timeout. Header-only.

#ifndef LAUFEY_CONTROLLER_RETRY_H_
#define LAUFEY_CONTROLLER_RETRY_H_

#include <cstdint>

namespace laufey_common {

// The whole retry window, from the first failure.
constexpr uint32_t kControllerRetryBudgetMs = 30000;
constexpr uint32_t kControllerRetryFirstDelayMs = 250;
constexpr uint32_t kControllerRetryMaxDelayMs = 2000;

// The delay before retry number `failures` (1 for the first failure), or 0
// when no retry is due: `invalid_state` (the error that is never retried),
// or the delay would end past the budget (`elapsed_ms` since the first
// failure).
inline uint32_t ControllerRetryDelayMs(uint32_t failures, uint32_t elapsed_ms,
                                       bool invalid_state) {
  if (invalid_state || failures == 0)
    return 0;
  uint32_t delay = kControllerRetryMaxDelayMs;
  if (failures <= 4)
    delay = kControllerRetryFirstDelayMs << (failures - 1);
  if (delay > kControllerRetryMaxDelayMs)
    delay = kControllerRetryMaxDelayMs;
  if (static_cast<uint64_t>(elapsed_ms) + delay > kControllerRetryBudgetMs)
    return 0;
  return delay;
}

}  // namespace laufey_common

#endif  // LAUFEY_CONTROLLER_RETRY_H_
