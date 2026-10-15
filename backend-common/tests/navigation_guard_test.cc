// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_navigation_guard.h: what a window's top-level frame may load (API
// 48), the allowed-origin entries, and the open-external capture the e2e
// battery reads. Plain strings, no engine. Plain asserts, no framework.

#include "laufey_navigation_guard.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
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
  const NavigationDecision kAllow = NavigationDecision::kAllow;
  const NavigationDecision kOpen = NavigationDecision::kOpenInBrowser;
  const NavigationDecision kBlock = NavigationDecision::kBlock;
  const std::vector<std::string> none;

  // --- A window's own key ----------------------------------------------------
  EXPECT(NavigationOriginKey("app://localhost/index.html") ==
         "app://localhost");
  EXPECT(NavigationOriginKey("MyApp://App/x?y#z") == "myapp://app");
  EXPECT(NavigationOriginKey("http://127.0.0.1:5173/") ==
         "http://127.0.0.1:5173");
  EXPECT(NavigationOriginKey("https://example.com:443/a") ==
         "https://example.com");
  EXPECT(NavigationOriginKey("file:///Users/x/index.html") == "file:");
  EXPECT(NavigationOriginKey("/Users/x/index.html") == "file:");
  EXPECT(NavigationOriginKey("//evil.example/x") == "");
  EXPECT(NavigationOriginKey("data:text/html,hi") == "");
  EXPECT(NavigationOriginKey("about:blank") == "");
  EXPECT(NavigationOriginKey("not a url") == "");

  // --- The app's origin stays in the window ----------------------------------
  {
    const std::string app = "app://localhost";
    EXPECT(DecideTopLevelNavigation(app, none, "app://localhost/") == kAllow);
    EXPECT(DecideTopLevelNavigation(app, none, "APP://LOCALHOST/route?q=1#h") ==
           kAllow);
    // A blob: URL has its creator's origin.
    EXPECT(DecideTopLevelNavigation(app, none, "blob:app://localhost/1234") ==
           kAllow);
    EXPECT(DecideTopLevelNavigation(app, none, "about:blank") == kAllow);
    EXPECT(DecideTopLevelNavigation(app, none, "about:srcdoc") == kAllow);
    EXPECT(DecideTopLevelNavigation(app, none, "About:Blank#x") == kAllow);
  }

  // --- Any other origin opens in the browser, or nowhere ---------------------
  {
    const std::string app = "app://localhost";
    EXPECT(DecideTopLevelNavigation(app, none, "https://example.com/") ==
           kOpen);
    EXPECT(DecideTopLevelNavigation(app, none, "http://127.0.0.1:8000/") ==
           kOpen);
    // Same scheme, another host.
    EXPECT(DecideTopLevelNavigation(app, none, "app://other/") == kBlock);
    // Lookalikes.
    EXPECT(DecideTopLevelNavigation(app, none, "app://localhost.evil/") ==
           kBlock);
    EXPECT(DecideTopLevelNavigation(
               app, none, "https://localhost@evil.example/") == kOpen);
    // Never handed to the OS: they could start a program or open a file.
    EXPECT(DecideTopLevelNavigation(app, none, "file:///etc/passwd") == kBlock);
    EXPECT(DecideTopLevelNavigation(app, none, "data:text/html,x") == kBlock);
    EXPECT(DecideTopLevelNavigation(app, none, "javascript:alert(1)") ==
           kBlock);
    EXPECT(DecideTopLevelNavigation(app, none, "mailto:a@example.com") ==
           kBlock);
    EXPECT(DecideTopLevelNavigation(app, none, "ms-settings:privacy") ==
           kBlock);
    EXPECT(DecideTopLevelNavigation(app, none, "\\\\server\\share") == kBlock);
  }

  // --- A loopback app (the stock runtime's shape) ----------------------------
  {
    const std::string app = NavigationOriginKey("http://127.0.0.1:4507/");
    EXPECT(DecideTopLevelNavigation(app, none, "http://127.0.0.1:4507/x") ==
           kAllow);
    EXPECT(DecideTopLevelNavigation(app, none, "http://127.0.0.1:4508/") ==
           kOpen);
    EXPECT(DecideTopLevelNavigation(app, none, "http://localhost:4507/") ==
           kOpen);
    EXPECT(DecideTopLevelNavigation(app, none, "https://127.0.0.1:4507/") ==
           kOpen);
  }

  // --- A window the embedder never navigated --------------------------------
  {
    EXPECT(DecideTopLevelNavigation("", none, "about:blank") == kAllow);
    EXPECT(DecideTopLevelNavigation("", none, "https://example.com/") == kOpen);
    EXPECT(DecideTopLevelNavigation("", none, "app://localhost/") == kBlock);
  }

  // --- A file: app: other files, nothing else --------------------------------
  {
    EXPECT(DecideTopLevelNavigation("file:", none, "file:///a/b.html") ==
           kAllow);
    EXPECT(DecideTopLevelNavigation("file:", none, "https://example.com/") ==
           kOpen);
    EXPECT(DecideTopLevelNavigation("", none, "file:///a/b.html") == kBlock);
  }

  // --- The embedder's allowed origins ----------------------------------------
  {
    EXPECT(NormalizeAllowedNavigationOrigin("https://Auth.Example.com/") ==
           "https://auth.example.com");
    EXPECT(NormalizeAllowedNavigationOrigin("https://a.example:443") ==
           "https://a.example");
    EXPECT(NormalizeAllowedNavigationOrigin("http://127.0.0.1:9000") ==
           "http://127.0.0.1:9000");
    EXPECT(NormalizeAllowedNavigationOrigin("*") == "");
    EXPECT(NormalizeAllowedNavigationOrigin("https://*") == "");
    EXPECT(NormalizeAllowedNavigationOrigin("https://a.example/path") == "");
    EXPECT(NormalizeAllowedNavigationOrigin("https://a.example?x") == "");
    EXPECT(NormalizeAllowedNavigationOrigin("https://u@a.example") == "");
    EXPECT(NormalizeAllowedNavigationOrigin("a.example") == "");
    EXPECT(NormalizeAllowedNavigationOrigin("") == "");

    const std::vector<std::string> allowed = {"https://auth.example.com"};
    const std::string app = "app://localhost";
    EXPECT(DecideTopLevelNavigation(
               app, allowed, "https://auth.example.com/login") == kAllow);
    EXPECT(DecideTopLevelNavigation(app, allowed,
                                    "https://AUTH.example.com:443/") == kAllow);
    // Exact origins: no subdomain, scheme or port wildcard.
    EXPECT(DecideTopLevelNavigation(app, allowed,
                                    "https://x.auth.example.com/") == kOpen);
    EXPECT(DecideTopLevelNavigation(app, allowed, "http://auth.example.com/") ==
           kOpen);
    EXPECT(DecideTopLevelNavigation(app, allowed,
                                    "https://auth.example.com:8443/") == kOpen);
  }

  // --- The process guard -----------------------------------------------------
  {
    NavigationGuard& guard = NavigationGuard::Instance();
    // Nothing navigated yet: only blank documents.
    EXPECT(guard.Decide(7, "app://localhost/") == kBlock);
    guard.SetWindowUrl(7, "app://localhost/index.html");
    EXPECT(guard.Decide(7, "app://localhost/next") == kAllow);
    EXPECT(guard.Decide(8, "app://localhost/next") == kBlock);
    // The embedder's own navigation moves the window's origin.
    guard.SetWindowUrl(7, "https://docs.example.com/");
    EXPECT(guard.Decide(7, "https://docs.example.com/page") == kAllow);
    EXPECT(guard.Decide(7, "app://localhost/") == kBlock);
    EXPECT(guard.SetAllowedOrigins({"app://localhost", "https://*", "x"}) == 1);
    EXPECT(guard.Decide(7, "app://localhost/") == kAllow);
    EXPECT(guard.Decide(8, "app://localhost/") == kAllow);
    EXPECT(guard.SetAllowedOrigins({}) == 0);
    EXPECT(guard.Decide(8, "app://localhost/") == kBlock);
    guard.ForgetWindow(7);
    EXPECT(guard.Decide(7, "https://docs.example.com/page") == kOpen);
  }

  // --- The per-window budget of browser opens --------------------------------
  {
    using Clock = ExternalOpenBudget::Clock;
    const Clock::time_point t0 = Clock::now();
    ExternalOpenBudget budget;
    EXPECT(budget.TryTake(t0));
    EXPECT(budget.TryTake(t0 + std::chrono::milliseconds(100)));
    EXPECT(budget.TryTake(t0 + std::chrono::milliseconds(200)));
    EXPECT(!budget.TryTake(t0 + std::chrono::milliseconds(300)));
    EXPECT(!budget.TryTake(t0 + std::chrono::milliseconds(999)));
    // A second after the first open, one slot is free again.
    EXPECT(budget.TryTake(t0 + std::chrono::milliseconds(1000)));
    EXPECT(!budget.TryTake(t0 + std::chrono::milliseconds(1050)));

    NavigationGuard& guard = NavigationGuard::Instance();
    guard.SetWindowUrl(42, "app://localhost/");
    const std::string away = "https://away.example/";
    for (size_t i = 0; i < kMaxExternalOpensPerSecond; ++i)
      EXPECT(guard.Decide(42, away, t0) == kOpen);
    EXPECT(guard.Decide(42, away, t0) == NavigationDecision::kThrottled);
    // Other windows have budgets of their own.
    guard.SetWindowUrl(43, "app://localhost/");
    EXPECT(guard.Decide(43, away, t0) == kOpen);
    // Navigations that stay in the window, or are dropped, cost nothing.
    EXPECT(guard.Decide(42, "app://localhost/x", t0) == kAllow);
    EXPECT(guard.Decide(42, "file:///x", t0) == kBlock);
    EXPECT(guard.Decide(42, away, t0 + std::chrono::seconds(1)) == kOpen);
    guard.ForgetWindow(42);
    guard.ForgetWindow(43);
  }

  // --- The open-external capture --------------------------------------------
  {
    ExternalOpenCapture& capture = ExternalOpenCapture::Instance();
    EXPECT(!capture.Capture("https://a.example/"));
    EXPECT(capture.Take().empty());
    capture.SetEnabled(true);
    EXPECT(capture.Capture("https://a.example/"));
    EXPECT(capture.Capture("https://b.example/\"q\\"));
    std::vector<std::string> taken = capture.Take();
    EXPECT(taken.size() == 2);
    EXPECT(capture.Take().empty());
    EXPECT(ExternalOpensJson(taken) ==
           "[\"https://a.example/\",\"https://b.example/\\\"q\\\\\"]");
    EXPECT(ExternalOpensJson({}) == "[]");
    EXPECT(ExternalOpensJson({std::string("a\nb")}) == "[\"a\\u000ab\"]");
    capture.Capture("https://c.example/");
    capture.SetEnabled(false);
    EXPECT(capture.Take().empty());
    EXPECT(!capture.Capture("https://d.example/"));
  }

  std::printf("navigation_guard_test: ok\n");
  return 0;
}
