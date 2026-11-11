// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_will_navigate.h (API 56): what is reported, each navigation decided
// exactly once (answer, deadline, closed window, cleared handler), and the
// embedder / re-issue tokens. No OS.

#include "laufey_will_navigate.h"

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

struct Reported {
  uint32_t window_id;
  uint64_t id;
  std::string url;
  std::string initiator;
  bool had_initiator;
  uint32_t kind;
};
static std::vector<Reported> g_reported;

static void Handler(void* user_data, uint32_t window_id, uint64_t id,
                    const char* url, const char* initiator, uint32_t kind) {
  EXPECT(user_data == &g_reported);
  g_reported.push_back(Reported{window_id, id, url ? url : "",
                                initiator ? initiator : "",
                                initiator != nullptr, kind});
}

int main() {
  // What is reported.
  EXPECT(WillNavigateReported("myapp://app/next"));
  EXPECT(WillNavigateReported("https://example.com/"));
  EXPECT(!WillNavigateReported("about:blank"));
  EXPECT(!WillNavigateReported("ABOUT:BLANK#top"));
  EXPECT(!WillNavigateReported("about:srcdoc"));
  EXPECT(!WillNavigateReported(""));

  // Spellings an engine normalizes match.
  EXPECT(NavigationMatchKey("MyApp://App") == "myapp://app/");
  EXPECT(NavigationMatchKey("myapp://app/") == "myapp://app/");
  EXPECT(NavigationMatchKey("myapp://app?x=1") == "myapp://app/?x=1");
  EXPECT(NavigationMatchKey("https://A.example/Path#frag") ==
         "https://a.example/Path");
  EXPECT(NavigationMatchKey("about:blank") == "about:blank");
  EXPECT(NavigationMatchKey("https://a/x") !=
         NavigationMatchKey("https://a/y"));

  EXPECT(NormalizeWillNavigateDecision(LAUFEY_NAVIGATION_ALLOW) ==
         LAUFEY_NAVIGATION_ALLOW);
  EXPECT(NormalizeWillNavigateDecision(LAUFEY_NAVIGATION_REFUSE) ==
         LAUFEY_NAVIGATION_REFUSE);
  EXPECT(NormalizeWillNavigateDecision(77) == LAUFEY_NAVIGATION_DEFAULT);

  auto& r = WillNavigateRequests::Instance();
  std::vector<uint32_t> replies;
  auto record = [&replies](uint32_t d) { replies.push_back(d); };
  const auto never = std::chrono::milliseconds(0);

  // No handler: nothing is reported, the reply never runs.
  EXPECT(r.Ask(1, "https://a/", "", LAUFEY_NAVIGATION_KIND_LINK, record,
               never) == WillNavigateStart::kNoHandler);
  EXPECT(replies.empty() && g_reported.empty() && r.PendingCount() == 0);

  r.SetHandler(Handler, &g_reported);
  EXPECT(r.HandlerSet());

  // Reported with its details, answered once.
  EXPECT(r.Ask(1, "https://a/x", "myapp://app/", LAUFEY_NAVIGATION_KIND_LINK,
               record, never) == WillNavigateStart::kAsked);
  EXPECT(g_reported.size() == 1);
  EXPECT(g_reported[0].window_id == 1 && g_reported[0].url == "https://a/x");
  EXPECT(g_reported[0].had_initiator &&
         g_reported[0].initiator == "myapp://app/");
  EXPECT(g_reported[0].kind == LAUFEY_NAVIGATION_KIND_LINK);
  EXPECT(replies.empty() && r.PendingCount() == 1);
  EXPECT(r.Answer(g_reported[0].id, LAUFEY_NAVIGATION_ALLOW));
  EXPECT(replies.size() == 1 && replies[0] == LAUFEY_NAVIGATION_ALLOW);
  EXPECT(!r.Answer(g_reported[0].id, LAUFEY_NAVIGATION_REFUSE));
  EXPECT(replies.size() == 1);

  // No initiator: NULL to the handler. An unknown answer is DEFAULT.
  replies.clear();
  r.Ask(2, "myapp://app/b", "", LAUFEY_NAVIGATION_KIND_SCRIPT, record, never);
  EXPECT(!g_reported.back().had_initiator);
  EXPECT(r.Answer(g_reported.back().id, 9));
  EXPECT(replies.size() == 1 && replies[0] == LAUFEY_NAVIGATION_DEFAULT);

  // The window closes: its navigations are refused, others wait on.
  replies.clear();
  r.Ask(3, "https://a/1", "", 0, record, never);
  uint64_t other = 0;
  r.Ask(4, "https://a/2", "", 0, [&other](uint32_t) { other = 1; }, never);
  uint64_t other_id = g_reported.back().id;
  r.ForgetWindow(3);
  EXPECT(replies.size() == 1 && replies[0] == LAUFEY_NAVIGATION_REFUSE);
  EXPECT(other == 0 && r.PendingCount() == 1);
  EXPECT(r.Answer(other_id, LAUFEY_NAVIGATION_REFUSE) && other == 1);

  // The deadline: DEFAULT (the guard decides), once.
  replies.clear();
  auto now = WillNavigateRequests::Clock::now();
  r.Ask(5, "https://a/slow", "", 0, record, std::chrono::hours(1));
  uint64_t slow = g_reported.back().id;
  EXPECT(r.ExpireDue(now) == 0 && replies.empty());
  EXPECT(r.ExpireDue(now + std::chrono::hours(2)) == 1);
  EXPECT(replies.size() == 1 && replies[0] == LAUFEY_NAVIGATION_DEFAULT);
  EXPECT(!r.Answer(slow, LAUFEY_NAVIGATION_ALLOW) && replies.size() == 1);

  // The timer thread expires a short deadline by itself.
  replies.clear();
  r.Ask(6, "https://a/timer", "", 0, record, std::chrono::milliseconds(20));
  for (int i = 0; i < 200 && r.PendingCount() > 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT(r.PendingCount() == 0);
  EXPECT(replies.size() == 1 && replies[0] == LAUFEY_NAVIGATION_DEFAULT);

  // Clearing the handler releases what waits to the guard.
  replies.clear();
  r.Ask(7, "https://a/c", "", 0, record, never);
  r.SetHandler(nullptr, nullptr);
  EXPECT(!r.HandlerSet());
  EXPECT(replies.size() == 1 && replies[0] == LAUFEY_NAVIGATION_DEFAULT);

  // The embedder's own navigation: one token per window, matched on the
  // normalized URL, consumed once, expired after its lifetime, dropped with
  // its window.
  auto t0 = WillNavigateRequests::Clock::now();
  r.ExpectEmbedderNavigation(8, "MyApp://App", t0);
  EXPECT(!r.TakeEmbedderNavigation(8, "myapp://app/other", t0));
  EXPECT(!r.TakeEmbedderNavigation(9, "myapp://app/", t0));
  EXPECT(r.TakeEmbedderNavigation(8, "myapp://app/", t0));
  EXPECT(!r.TakeEmbedderNavigation(8, "myapp://app/", t0));
  r.ExpectEmbedderNavigation(8, "myapp://app/", t0);
  EXPECT(!r.TakeEmbedderNavigation(
      8, "myapp://app/",
      t0 + kWillNavigateTokenLifetime + std::chrono::milliseconds(1)));
  r.ExpectEmbedderNavigation(8, "myapp://app/", t0);
  r.ForgetWindow(8);
  EXPECT(!r.TakeEmbedderNavigation(8, "myapp://app/", t0));

  // Re-issue tokens are kept apart from the embedder's.
  r.ExpectReissue(10, "https://a/form", t0);
  EXPECT(!r.TakeEmbedderNavigation(10, "https://a/form", t0));
  EXPECT(r.TakeReissue(10, "https://a/form", true, t0));
  EXPECT(!r.TakeReissue(10, "https://a/form", true, t0));

  // A re-issue token is bound to the load the backend starts: a page's own
  // navigation to the same URL (not started by the backend) does not match,
  // and it spends the token, so the backend's load that follows is asked
  // again rather than a later page navigation getting through.
  r.ExpectReissue(11, "https://a/page", t0);
  EXPECT(!r.TakeReissue(11, "https://a/page", false, t0));
  EXPECT(!r.TakeReissue(11, "https://a/page", true, t0));
  // The window's next navigation start spends it, matching or not.
  r.ExpectReissue(12, "https://a/one", t0);
  EXPECT(!r.TakeReissue(12, "https://a/two", true, t0));
  EXPECT(!r.TakeReissue(12, "https://a/one", true, t0));
  // Expired: no match.
  r.ExpectReissue(13, "https://a/late", t0);
  EXPECT(!r.TakeReissue(
      13, "https://a/late", true,
      t0 + kWillNavigateTokenLifetime + std::chrono::milliseconds(1)));
  // Other windows keep theirs.
  r.ExpectReissue(14, "https://a/x", t0);
  EXPECT(!r.TakeReissue(15, "https://a/x", true, t0));
  EXPECT(r.TakeReissue(14, "https://a/x", true, t0));

  // CEF and WebView2 never hold a non-GET navigation (it would be sent
  // again on the window's authority): GET only.
  EXPECT(WillNavigateHoldsMethod("GET"));
  EXPECT(WillNavigateHoldsMethod("get"));
  EXPECT(WillNavigateHoldsMethod(""));
  EXPECT(!WillNavigateHoldsMethod("POST"));
  EXPECT(!WillNavigateHoldsMethod("PUT"));
  EXPECT(!WillNavigateHoldsMethod("GETX"));
  EXPECT(!WillNavigateHoldsMethod("HEAD"));
  // WebView2 reports headers, not the method.
  EXPECT(!NavigationHeadersMarkNonGet({}));
  EXPECT(!NavigationHeadersMarkNonGet({"Accept", "User-Agent", "Referer"}));
  EXPECT(NavigationHeadersMarkNonGet({"Accept", "Content-Type"}));
  EXPECT(NavigationHeadersMarkNonGet({"ORIGIN"}));
  EXPECT(HeaderNamesContain({"Accept", "REFERER"}, "referer"));
  EXPECT(!HeaderNamesContain({"Referrer", "Refere"}, "referer"));

  std::printf("will_navigate_test: all passed\n");
  return 0;
}
