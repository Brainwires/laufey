// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for laufey_scheme_registry.h. Plain asserts, no framework: run
// via `ctest --test-dir webview/build` (or cef/build). Exits non-zero on the
// first failure.

#include "laufey_scheme_registry.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using laufey_common::IsValidSchemeName;
using laufey_common::NormalizeSchemeName;
using laufey_common::ParseSchemeList;
using laufey_common::SchemeRegistry;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

static void TestSchemeNameGrammar() {
  EXPECT(IsValidSchemeName("app"));
  EXPECT(IsValidSchemeName("t3code"));
  EXPECT(IsValidSchemeName("my-app.v2+x"));
  EXPECT(IsValidSchemeName("A"));
  EXPECT(IsValidSchemeName("MyApp"));

  EXPECT(!IsValidSchemeName(""));
  EXPECT(!IsValidSchemeName("1app"));  // must start with a letter
  EXPECT(!IsValidSchemeName("-app"));
  EXPECT(!IsValidSchemeName("app://"));  // name only, no separator
  EXPECT(!IsValidSchemeName("my app"));
  EXPECT(!IsValidSchemeName("my_app"));  // "_" is not in the RFC grammar
  EXPECT(!IsValidSchemeName("app/x"));
  EXPECT(!IsValidSchemeName(std::string("app\0x", 5)));

  EXPECT(NormalizeSchemeName("MyApp") == "myapp");
  EXPECT(NormalizeSchemeName("app") == "app");
  EXPECT(NormalizeSchemeName("T3-Code.X+Y") == "t3-code.x+y");
}

static void TestRegistryDefaults() {
  SchemeRegistry registry;
  EXPECT(registry.size() == 1);
  EXPECT(registry.Contains("app"));
  EXPECT(registry.Contains("APP"));
  EXPECT(!registry.Contains("t3code"));
  std::vector<std::string> snapshot = registry.Snapshot();
  EXPECT(snapshot.size() == 1);
  EXPECT(snapshot[0] == "app");
}

static void TestRegistryAdd() {
  SchemeRegistry registry;

  // "app" is pre-registered: re-adding it is a no-op.
  EXPECT(!registry.Add("app"));
  EXPECT(!registry.Add("APP"));
  EXPECT(registry.size() == 1);

  // A new scheme reports true once, then false (already present), and is
  // stored lowercase regardless of how it was spelled.
  EXPECT(registry.Add("t3code"));
  EXPECT(!registry.Add("t3code"));
  EXPECT(!registry.Add("T3Code"));
  EXPECT(registry.Contains("t3code"));
  EXPECT(registry.Contains("T3CODE"));

  // Invalid names are rejected without being stored.
  EXPECT(!registry.Add(""));
  EXPECT(!registry.Add("bad scheme"));
  EXPECT(!registry.Add("app://"));
  EXPECT(!registry.Contains("bad scheme"));

  // Snapshot keeps "app" first and then registration order.
  EXPECT(registry.Add("Zeta"));
  EXPECT(registry.Add("alpha"));
  std::vector<std::string> snapshot = registry.Snapshot();
  EXPECT(snapshot.size() == 4);
  EXPECT(snapshot[0] == "app");
  EXPECT(snapshot[1] == "t3code");
  EXPECT(snapshot[2] == "zeta");
  EXPECT(snapshot[3] == "alpha");
  EXPECT(registry.size() == 4);
}

static void TestRegistryConcurrentAdd() {
  // The runtime registers from its own thread while the UI thread snapshots.
  // Hammer Add/Snapshot from several threads and check the final set is
  // exactly the union, with each name present once.
  SchemeRegistry registry;
  constexpr int kThreads = 8;
  constexpr int kPerThread = 200;
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&registry, t]() {
      for (int i = 0; i < kPerThread; ++i) {
        // Overlapping names across threads: only one Add per name wins.
        registry.Add("s" + std::to_string(i % 50));
        registry.Add("thread" + std::to_string(t));
        std::vector<std::string> snapshot = registry.Snapshot();
        EXPECT(!snapshot.empty() && snapshot[0] == "app");
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT(registry.size() == 1 + 50 + kThreads);
  for (int i = 0; i < 50; ++i) {
    EXPECT(registry.Contains("s" + std::to_string(i)));
  }
  for (int t = 0; t < kThreads; ++t) {
    EXPECT(registry.Contains("thread" + std::to_string(t)));
  }
}

static void TestGlobalInstance() {
  SchemeRegistry* a = SchemeRegistry::GetInstance();
  SchemeRegistry* b = SchemeRegistry::GetInstance();
  EXPECT(a == b);
  EXPECT(a->Contains("app"));
}

static void TestParseSchemeList() {
  std::vector<std::string> rejected;

  std::vector<std::string> empty = ParseSchemeList("", &rejected);
  EXPECT(empty.empty());
  EXPECT(rejected.empty());

  std::vector<std::string> one = ParseSchemeList("t3code", &rejected);
  EXPECT(one.size() == 1 && one[0] == "t3code");

  // Whitespace, case, duplicates and empty entries.
  std::vector<std::string> many =
      ParseSchemeList(" T3Code , other,,t3code, third ", &rejected);
  EXPECT(many.size() == 3);
  EXPECT(many[0] == "t3code");
  EXPECT(many[1] == "other");
  EXPECT(many[2] == "third");
  EXPECT(rejected.empty());

  // Invalid entries are reported and skipped; valid neighbours survive.
  std::vector<std::string> mixed =
      ParseSchemeList("good,1bad,also good,app://,fine", &rejected);
  EXPECT(mixed.size() == 2);
  EXPECT(mixed[0] == "good");
  EXPECT(mixed[1] == "fine");
  EXPECT(rejected.size() == 3);
  EXPECT(rejected[0] == "1bad");
  EXPECT(rejected[1] == "also good");
  EXPECT(rejected[2] == "app://");

  // A null `rejected` is allowed.
  std::vector<std::string> quiet = ParseSchemeList("ok,not ok");
  EXPECT(quiet.size() == 1 && quiet[0] == "ok");
}

int main() {
  TestSchemeNameGrammar();
  TestRegistryDefaults();
  TestRegistryAdd();
  TestRegistryConcurrentAdd();
  TestGlobalInstance();
  TestParseSchemeList();
  std::printf("scheme_registry_test: all tests passed\n");
  return 0;
}
