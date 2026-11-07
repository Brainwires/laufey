// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// PendingEvals (laufey_pending_evals.h): an evaluation's result settles it
// only when it comes from the browser the script was sent to; one from
// another browser, an id never issued, or a second answer settles nothing,
// and the evaluation still takes its own browser's answer. Plain asserts, no
// framework.

#include "laufey_pending_evals.h"

#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <thread>
#include <vector>

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using laufey_common::PendingEvals;

int main() {
  {
    PendingEvals<std::string> evals;
    uint64_t a = evals.Add(1, "a");
    uint64_t b = evals.Add(2, "b");
    EXPECT(a != 0 && b != 0 && a != b);
    std::string out;
    // Browser 2's renderer answers browser 1's evaluation: nothing settles.
    EXPECT(!evals.Take(a, 2, &out));
    EXPECT(evals.size() == 2);
    // Browser 1's own answer still does.
    EXPECT(evals.Take(a, 1, &out) && out == "a");
    EXPECT(evals.Take(b, 2, &out) && out == "b");
    // A second answer, and ids never issued, settle nothing.
    EXPECT(!evals.Take(a, 1, &out));
    EXPECT(!evals.Take(0, 1, &out));
    EXPECT(!evals.Take(987654321, 1, &out));
    EXPECT(evals.size() == 0);
  }
  {
    // Ids are never reused.
    PendingEvals<int> evals;
    std::set<uint64_t> ids;
    for (int i = 0; i < 100; ++i) {
      uint64_t id = evals.Add(5, i);
      EXPECT(evals.Take(id, 5, nullptr));
      ids.insert(id);
    }
    EXPECT(ids.size() == 100);
  }
  {
    // Any thread: concurrent adds and takes stay consistent.
    PendingEvals<int> evals;
    std::vector<std::thread> threads;
    for (int w = 1; w <= 8; ++w) {
      threads.emplace_back([&evals, w] {
        for (int i = 1; i <= 2000; ++i) {
          uint64_t id = evals.Add(w, i);
          int out = 0;
          EXPECT(!evals.Take(id, w + 100, &out));
          EXPECT(evals.Take(id, w, &out) && out == i);
        }
      });
    }
    for (auto& t : threads)
      t.join();
    EXPECT(evals.size() == 0);
  }
  std::printf("pending_evals_test: ok\n");
  return 0;
}
