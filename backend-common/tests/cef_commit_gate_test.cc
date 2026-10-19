// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_cef_commit_gate.h: which responses hold their body for the
// document's commit, and the gates a commit, a deadline, a cancel and a
// closing browser release. No engine. Plain asserts, no framework.

#include "laufey_cef_commit_gate.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
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

using namespace laufey_common;

int main() {
  // --- Which bodies wait --------------------------------------------------
  EXPECT(DocumentBodyNeedsCommitGate(true, 200, "text/html"));
  EXPECT(DocumentBodyNeedsCommitGate(true, 200, "Text/HTML"));
  EXPECT(DocumentBodyNeedsCommitGate(true, 203, "application/xhtml+xml"));
  EXPECT(DocumentBodyNeedsCommitGate(true, 200, "image/svg+xml"));
  EXPECT(DocumentBodyNeedsCommitGate(true, 200, "text/xml"));
  EXPECT(DocumentBodyNeedsCommitGate(true, 200, "application/xml"));
  // A subresource (fetch, script, image) never waits.
  EXPECT(!DocumentBodyNeedsCommitGate(false, 200, "text/html"));
  // No document that runs scripts, or no body that commits a document.
  EXPECT(!DocumentBodyNeedsCommitGate(true, 200, "image/png"));
  EXPECT(!DocumentBodyNeedsCommitGate(true, 200, "application/json"));
  EXPECT(!DocumentBodyNeedsCommitGate(true, 200, "application/pdf"));
  EXPECT(!DocumentBodyNeedsCommitGate(true, 204, "text/html"));
  EXPECT(!DocumentBodyNeedsCommitGate(true, 205, "text/html"));
  EXPECT(!DocumentBodyNeedsCommitGate(true, 302, "text/html"));
  EXPECT(!DocumentBodyNeedsCommitGate(true, 404, "text/html"));
  EXPECT(!DocumentBodyNeedsCommitGate(true, 500, "text/html"));
  // No type: the browser sniffs the body before it commits.
  EXPECT(!DocumentBodyNeedsCommitGate(true, 200, ""));

  EXPECT(CommitGateUrlKey("app://x/a?b#c") == "app://x/a?b");
  EXPECT(CommitGateUrlKey("app://x/") == "app://x/");

  // --- A main frame's commit ----------------------------------------------
  {
    DocumentCommitGates gates;
    int a = 0, b = 0, other = 0, sub = 0;
    gates.Add(1, true, "app://x/", [&] { ++a; });
    gates.Add(1, true, "app://x/other", [&] { ++b; });
    gates.Add(2, true, "app://x/", [&] { ++other; });
    gates.Add(1, false, "app://x/", [&] { ++sub; });
    // The window's initial about:blank commits first: nothing goes.
    EXPECT(gates.Committed(1, true, "about:blank") == 0);
    EXPECT(a == 0);
    EXPECT(gates.Committed(1, true, "app://x/#top") == 1);
    EXPECT(a == 1 && b == 0 && other == 0 && sub == 0);
    // Released once.
    EXPECT(gates.Committed(1, true, "app://x/") == 0);
    EXPECT(a == 1);
    EXPECT(gates.HeldForTesting() == 3);
  }

  // --- A sub-frame's commit releases the gates for its URL ----------------
  {
    DocumentCommitGates gates;
    int one = 0, two = 0;
    gates.Add(1, false, "app://x/one", [&] { ++one; });
    gates.Add(1, false, "app://x/two", [&] { ++two; });
    EXPECT(gates.Committed(1, false, "app://x/two#f") == 1);
    EXPECT(one == 0 && two == 1);
    // A main frame's commit doesn't touch sub-frame gates.
    EXPECT(gates.Committed(1, true, "app://x/one") == 0);
    EXPECT(one == 0);
  }

  // --- The deadline, a cancel, a closing browser --------------------------
  {
    DocumentCommitGates gates;
    int late = 0, cancelled = 0, closing = 0;
    uint64_t l = gates.Add(1, true, "app://x/", [&] { ++late; });
    uint64_t c = gates.Add(3, true, "app://y/", [&] { ++cancelled; });
    gates.Add(4, false, "app://z/", [&] { ++closing; });
    EXPECT(l != 0 && c != 0 && l != c);
    EXPECT(gates.ReleaseOne(l));
    EXPECT(late == 1);
    EXPECT(!gates.ReleaseOne(l));  // already released
    EXPECT(late == 1);
    gates.Forget(c);  // a cancelled request is not released
    EXPECT(!gates.ReleaseOne(c));
    EXPECT(cancelled == 0);
    EXPECT(gates.ReleaseBrowser(4) == 1);
    EXPECT(closing == 1);
    EXPECT(gates.HeldForTesting() == 0);
  }

  // --- A release may add a gate (it runs outside the lock) ----------------
  {
    DocumentCommitGates gates;
    int inner = 0;
    gates.Add(1, true, "app://x/",
              [&] { gates.Add(1, true, "app://x/next", [&] { ++inner; }); });
    EXPECT(gates.Committed(1, true, "app://x/") == 1);
    EXPECT(gates.HeldForTesting() == 1);
    EXPECT(gates.Committed(1, true, "app://x/next") == 1);
    EXPECT(inner == 1);
  }

  // --- A commit and the deadline at once: one release ---------------------
  for (int round = 0; round < 200; ++round) {
    DocumentCommitGates gates;
    std::atomic<int> released{0};
    uint64_t id = gates.Add(7, true, "app://x/", [&] { ++released; });
    std::thread deadline([&] { gates.ReleaseOne(id); });
    gates.Committed(7, true, "app://x/");
    deadline.join();
    EXPECT(released == 1);
  }

  std::printf("cef_commit_gate_test: ok\n");
  return 0;
}
