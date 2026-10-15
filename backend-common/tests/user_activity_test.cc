// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_user_activity.h: the activity (API 49), the continued-activity
// JSON, and the dispatch that holds activities until a handler registers.

#include "laufey_user_activity.h"

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

static std::vector<std::string> g_seen;

int main() {
  UserActivity a;
  std::string error;
  EXPECT(ParseUserActivity(
      R"({"type":"com.example.view","title":"Doc","url":"https://e.example/d",)"
      R"("userInfo":{"id":"42"},"keywords":["a","b"],"handoff":false,)"
      R"("searchable":true})",
      &a, &error));
  EXPECT(a.type == "com.example.view" && a.title == "Doc" &&
         a.url == "https://e.example/d" && a.user_info.at("id") == "42" &&
         a.keywords.size() == 2 && !a.handoff && a.searchable);
  EXPECT(ParseUserActivity(R"({"type":"com.example.min"})", &a, &error));
  EXPECT(a.handoff && !a.searchable && a.url.empty());

  for (const char* bad : {
           "", "[]", "{}", R"({"type":"nodots"})", R"({"type":".a.b"})",
           R"({"type":"a.b c"})", R"({"type":"a.b","url":"file:///x"})",
           R"({"type":"a.b","userInfo":{"n":1}})",
           R"({"type":"a.b","keywords":"x"})", R"({"type":"a.b","handoff":1})",
       }) {
    UserActivity scratch;
    EXPECT(!ParseUserActivity(bad, &scratch, &error));
  }

  UserActivity c;
  c.type = "com.example.view";
  c.title = "T\"q";
  c.user_info["k"] = "v";
  EXPECT(UserActivityJson(c) ==
         R"({"type":"com.example.view","title":"T\"q","url":null,"userInfo":{"k":"v"}})");

  // Held until a handler registers, then delivered in order.
  DispatchContinuedUserActivity(c);
  c.title = "second";
  DispatchContinuedUserActivity(c);
  SetUserActivityHandler(
      [](void*, const char* json) { g_seen.push_back(json); }, nullptr);
  EXPECT(g_seen.size() == 2);
  EXPECT(g_seen[0].find("T\\\"q") != std::string::npos);
  EXPECT(g_seen[1].find("second") != std::string::npos);
  DispatchContinuedUserActivity(c);
  EXPECT(g_seen.size() == 3);
  SetUserActivityHandler(nullptr, nullptr);

#if !defined(__APPLE__)
  EXPECT(!SetUserActivityForAbi(R"({"type":"com.example.view"})"));
#endif
  std::printf("user_activity_test: ok\n");
  return 0;
}
