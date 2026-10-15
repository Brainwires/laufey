// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_share.h: the share request (API 49). Plain strings, no OS.

#include "laufey_share.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using namespace laufey_common;

static bool Ok(const std::string& json, ShareRequest* r = nullptr) {
  ShareRequest scratch;
  std::string error;
  return ParseShareRequest(json, r ? r : &scratch, &error);
}

int main() {
  ShareRequest r;
  EXPECT(Ok(R"({"title":"T","text":"hello","url":"https://a.example/x"})",
            &r));
  EXPECT(r.title == "T" && r.text == "hello" &&
         r.url == "https://a.example/x" && !r.has_point && r.files.empty());
  EXPECT(Ok(R"({"text":"x","x":10,"y":20})", &r));
  EXPECT(r.has_point && r.x == 10 && r.y == 20);
#if defined(_WIN32)
  EXPECT(Ok(R"({"files":["C:\\tmp\\a.txt","\\\\server\\share\\b"]})", &r));
  EXPECT(!Ok(R"({"files":["/tmp/a.txt"]})"));
#else
  EXPECT(Ok(R"({"files":["/tmp/a.txt"]})", &r));
  EXPECT(r.files.size() == 1 && r.files[0] == "/tmp/a.txt");
  EXPECT(!Ok(R"({"files":["relative.txt"]})"));
#endif
  // Unknown keys are ignored.
  EXPECT(Ok(R"({"text":"x","future":true})"));

  std::string error;
  for (const char* bad : {
           "", "[]", "{}", R"({"title":"only a title"})",
           R"({"text":1})", R"({"url":"no scheme"})",
           R"({"url":"https://a\u0007b"})", R"({"files":"x"})",
           R"({"files":[1]})", R"({"text":"x","x":1})",
           R"({"text":"x","x":1.5,"y":2})", R"({"text":"x","y":"2","x":1})",
       }) {
    ShareRequest scratch;
    EXPECT(!ParseShareRequest(bad, &scratch, &error));
    EXPECT(!error.empty());
  }

  // The refusal report: synchronous, with the reason.
  int got = -1;
  std::string detail;
  struct Ctx {
    int* got;
    std::string* detail;
  } ctx{&got, &detail};
  ShareReport(
      [](void* ud, int status, const char* d) {
        auto* c = static_cast<Ctx*>(ud);
        *c->got = status;
        *c->detail = d ? d : "";
      },
      &ctx, LAUFEY_SHARE_NOT_SUPPORTED, "why");
  EXPECT(got == LAUFEY_SHARE_NOT_SUPPORTED && detail == "why");

  std::printf("share_test: ok\n");
  return 0;
}
