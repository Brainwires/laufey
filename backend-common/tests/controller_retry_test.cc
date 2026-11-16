// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// ControllerRetryDelayMs (laufey_controller_retry.h): a failed WebView2
// controller creation is retried with a backoff for a budget of time, not a
// fixed number of tries, so a user data folder held for seconds by a browser
// process that is shutting down (a starved machine, an instance that was
// killed) still gets its window a web view. Plain asserts, no framework.

#include "laufey_controller_retry.h"

#include <cstdio>
#include <cstdlib>

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using laufey_common::ControllerRetryDelayMs;
using laufey_common::kControllerRetryBudgetMs;

int main() {
  // Backs off: 250, 500, 1000, 2000 ms, then 2 s apart.
  EXPECT(ControllerRetryDelayMs(1, 0, false) == 250);
  EXPECT(ControllerRetryDelayMs(2, 250, false) == 500);
  EXPECT(ControllerRetryDelayMs(3, 750, false) == 1000);
  EXPECT(ControllerRetryDelayMs(4, 1750, false) == 2000);
  EXPECT(ControllerRetryDelayMs(5, 3750, false) == 2000);
  EXPECT(ControllerRetryDelayMs(40, 9000, false) == 2000);
  // A folder held for 20 s (the brain run: ERROR_BUSY on every one of the
  // four fixed tries, 1.75 s in all) is still within the budget.
  uint32_t elapsed = 0;
  uint32_t failures = 0;
  while (elapsed < 20000) {
    uint32_t delay = ControllerRetryDelayMs(++failures, elapsed, false);
    EXPECT(delay > 0);
    elapsed += delay;
  }
  // The budget ends it: no retry that would end past 30 s.
  EXPECT(ControllerRetryDelayMs(20, kControllerRetryBudgetMs - 1000, false) ==
         0);
  EXPECT(ControllerRetryDelayMs(20, kControllerRetryBudgetMs - 2000, false) ==
         2000);
  // ERROR_INVALID_STATE is never retried; nothing before a failure.
  EXPECT(ControllerRetryDelayMs(1, 0, true) == 0);
  EXPECT(ControllerRetryDelayMs(0, 0, false) == 0);
  std::printf("controller_retry_test: ok\n");
  return 0;
}
