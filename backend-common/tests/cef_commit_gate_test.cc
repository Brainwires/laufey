// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_cef_commit_gate.h: which responses hold their body for the
// document's commit, and the gates a commit, a deadline, a cancel and a
// closing browser release. No engine. Plain asserts, no framework.

#include "laufey_cef_commit_gate.h"

#include <atomic>
#include <chrono>
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
using std::chrono::hours;
using std::chrono::milliseconds;
using std::chrono::seconds;

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
  // A download never commits: its body doesn't wait out the deadline.
  EXPECT(!DocumentBodyNeedsCommitGate(true, 200, "text/html", "attachment"));
  EXPECT(!DocumentBodyNeedsCommitGate(true, 200, "text/html",
                                      "Attachment; filename=\"a.html\""));
  EXPECT(!DocumentBodyNeedsCommitGate(true, 200, "application/xml",
                                      "  attachment ;filename=a.xml"));
  EXPECT(DocumentBodyNeedsCommitGate(true, 200, "text/html", "inline"));
  EXPECT(DocumentBodyNeedsCommitGate(true, 200, "text/html",
                                     "INLINE; filename=a.html"));
  EXPECT(DocumentBodyNeedsCommitGate(true, 200, "text/html", ""));

  // --- Content-Disposition as Chromium reads it ---------------------------
  EXPECT(ContentDispositionIsDownload("attachment"));
  EXPECT(ContentDispositionIsDownload("attachment; filename=a.html"));
  // Any other token downloads too (HttpContentDisposition).
  EXPECT(ContentDispositionIsDownload("form-data; name=x"));
  EXPECT(!ContentDispositionIsDownload("inline"));
  EXPECT(!ContentDispositionIsDownload(" Inline ;filename=a"));
  EXPECT(!ContentDispositionIsDownload(""));
  EXPECT(!ContentDispositionIsDownload("   "));
  // No type, only a parameter: renders.
  EXPECT(!ContentDispositionIsDownload("filename=a.html"));
  EXPECT(!ContentDispositionIsDownload("; filename=a.html"));
  EXPECT(!ContentDispositionIsDownload("\"attachment\""));

  EXPECT(CommitGateUrlKey("app://x/a?b#c") == "app://x/a?b");
  EXPECT(CommitGateUrlKey("app://x/") == "app://x/");

  // --- A main frame's commit ----------------------------------------------
  {
    DocumentCommitGates gates;
    int a = 0, b = 0, other = 0, sub = 0;
    gates.Add(1, true, "app://x/", [&](GateRelease) { ++a; });
    gates.Add(1, true, "app://x/other", [&](GateRelease) { ++b; });
    gates.Add(2, true, "app://x/", [&](GateRelease) { ++other; });
    gates.Add(1, false, "app://x/", [&](GateRelease) { ++sub; });
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
    gates.Add(1, false, "app://x/one", [&](GateRelease) { ++one; });
    gates.Add(1, false, "app://x/two", [&](GateRelease) { ++two; });
    EXPECT(gates.Committed(1, false, "app://x/two#f") == 1);
    EXPECT(one == 0 && two == 1);
    // A main frame's commit doesn't touch sub-frame gates.
    EXPECT(gates.Committed(1, true, "app://x/one") == 0);
    EXPECT(one == 0);
  }

  // --- The deadline, a cancel, a closing browser --------------------------
  const auto t0 = DocumentCommitGates::Clock::time_point() + seconds(100);
  const auto never = t0 + hours(1);
  {
    DocumentCommitGates gates;
    int late = 0, cancelled = 0, closing = 0;
    uint64_t l =
        gates.Add(1, true, "app://x/", [&](GateRelease) { ++late; }, t0);
    uint64_t c =
        gates.Add(3, true, "app://y/", [&](GateRelease) { ++cancelled; }, t0);
    gates.Add(4, false, "app://z/", [&](GateRelease) { ++closing; }, t0);
    EXPECT(l != 0 && c != 0 && l != c);
    EXPECT(gates.ReleaseIfDue(l, never) == milliseconds(0));
    EXPECT(late == 1);
    EXPECT(gates.ReleaseIfDue(l, never) == milliseconds(0));  // released once
    EXPECT(late == 1);
    gates.Forget(c);  // a cancelled request is not released
    EXPECT(gates.ReleaseIfDue(c, never) == milliseconds(0));
    EXPECT(gates.Committed(3, true, "app://y/") == 0);
    EXPECT(cancelled == 0);
    EXPECT(gates.ReleaseBrowser(4) == 1);
    EXPECT(closing == 1);
    EXPECT(gates.ReleaseBrowser(4) == 0);  // released once
    EXPECT(closing == 1);
    EXPECT(gates.HeldForTesting() == 0);
  }

  // --- How long a body waits for its commit -------------------------------
  // A commit can take seconds on a loaded machine (a renderer that starts
  // slowly): a body released before it leaves the page without
  // localStorage. At 1.5 s the body is still held, and the commit releases
  // it then.
  EXPECT(kCommitGateMaxWaitMs >= 10000);
  {
    DocumentCommitGates gates;
    int released = 0;
    uint64_t id =
        gates.Add(1, true, "app://x/", [&](GateRelease) { ++released; }, t0);
    EXPECT(gates.ReleaseIfDue(id, t0 + milliseconds(1000)) ==
           milliseconds(kCommitGateMaxWaitMs - 1000));
    EXPECT(gates.ReleaseIfDue(id, t0 + milliseconds(1500)) ==
           milliseconds(kCommitGateMaxWaitMs - 1500));
    EXPECT(released == 0);
    EXPECT(gates.Committed(1, true, "app://x/") == 1);
    EXPECT(released == 1);
    // The deadline after the commit releases nothing more.
    EXPECT(gates.ReleaseIfDue(id, never) == milliseconds(0));
    EXPECT(released == 1);
  }
  // A commit that never comes: the body goes at the deadline, not before,
  // and once.
  {
    DocumentCommitGates gates;
    int released = 0;
    uint64_t id =
        gates.Add(1, true, "app://x/", [&](GateRelease) { ++released; }, t0);
    const auto deadline = t0 + milliseconds(kCommitGateMaxWaitMs);
    EXPECT(gates.ReleaseIfDue(id, deadline - milliseconds(1)) ==
           milliseconds(1));
    EXPECT(released == 0);
    EXPECT(gates.ReleaseIfDue(id, deadline) == milliseconds(0));
    EXPECT(released == 1);
    EXPECT(gates.Committed(1, true, "app://x/") == 0);
    EXPECT(gates.ReleaseBrowser(1) == 0);
    EXPECT(released == 1);
  }
  // Another limit (the parameter): held up to it, released at it.
  {
    DocumentCommitGates gates;
    int released = 0;
    uint64_t id =
        gates.Add(1, true, "app://x/", [&](GateRelease) { ++released; }, t0);
    EXPECT(gates.ReleaseIfDue(id, t0 + milliseconds(40), milliseconds(50)) ==
           milliseconds(10));
    EXPECT(released == 0);
    EXPECT(gates.ReleaseIfDue(id, t0 + milliseconds(50), milliseconds(50)) ==
           milliseconds(0));
    EXPECT(released == 1);
  }
  // Each gate keeps its own clock: a later one isn't released with an
  // earlier one's deadline.
  {
    DocumentCommitGates gates;
    int first = 0, second = 0;
    uint64_t a =
        gates.Add(1, true, "app://x/", [&](GateRelease) { ++first; }, t0);
    uint64_t b = gates.Add(
        1, true, "app://x/", [&](GateRelease) { ++second; },
        t0 + milliseconds(5000));
    const auto at = t0 + milliseconds(kCommitGateMaxWaitMs);
    EXPECT(gates.ReleaseIfDue(a, at) == milliseconds(0));
    EXPECT(gates.ReleaseIfDue(b, at) == milliseconds(5000));
    EXPECT(first == 1 && second == 0);
  }

  // --- Each release says what released it --------------------------------
  // Only the deadline means the page went on without its commit; a commit
  // that lands after a long wait, or a closing browser, doesn't.
  {
    DocumentCommitGates gates;
    std::vector<GateRelease> how;
    auto record = [&](GateRelease h) { how.push_back(h); };
    uint64_t late = gates.Add(1, true, "app://x/late", record, t0);
    gates.Add(1, true, "app://x/slow", record, t0);
    gates.Add(2, true, "app://x/", record, t0);
    const auto after = t0 + milliseconds(kCommitGateMaxWaitMs + 5);
    EXPECT(gates.Committed(1, true, "app://x/slow") == 1);
    EXPECT(gates.ReleaseIfDue(late, after) == milliseconds(0));
    EXPECT(gates.ReleaseBrowser(2) == 1);
    EXPECT(how.size() == 3);
    EXPECT(how[0] == GateRelease::kCommitted);
    EXPECT(how[1] == GateRelease::kDeadline);
    EXPECT(how[2] == GateRelease::kBrowserClosed);
  }

  // --- A release may add a gate (it runs outside the lock) ----------------
  {
    DocumentCommitGates gates;
    int inner = 0;
    gates.Add(1, true, "app://x/", [&](GateRelease) {
      gates.Add(1, true, "app://x/next", [&](GateRelease) { ++inner; });
    });
    EXPECT(gates.Committed(1, true, "app://x/") == 1);
    EXPECT(gates.HeldForTesting() == 1);
    EXPECT(gates.Committed(1, true, "app://x/next") == 1);
    EXPECT(inner == 1);
  }

  // --- A commit and the deadline at once: one release ---------------------
  for (int round = 0; round < 200; ++round) {
    DocumentCommitGates gates;
    std::atomic<int> released{0};
    uint64_t id =
        gates.Add(7, true, "app://x/", [&](GateRelease) { ++released; });
    std::thread deadline([&] { gates.ReleaseIfDue(id, never); });
    gates.Committed(7, true, "app://x/");
    deadline.join();
    EXPECT(released == 1);
  }

  std::printf("cef_commit_gate_test: ok\n");
  return 0;
}
