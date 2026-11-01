// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_quit_request.h (API 52): who is asked, who answers, and that a
// request can't outlive its deadline's generation. No OS.

#include "laufey_quit_request.h"

#include <cstdio>
#include <cstdlib>
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

static std::vector<int32_t> g_asked;
static void Asked(int32_t kind, void*) {
  g_asked.push_back(kind);
}
static void AnswersAtOnce(int32_t, void*) {
  AnswerQuitRequest(false);
}

int main() {
  std::vector<bool> replies;
  auto record = [&replies](bool allow) { replies.push_back(allow); };

  // No handler: nobody to ask, and the reply is not called.
  EXPECT(BeginQuitRequest(LAUFEY_QUIT_REQUEST_APP, record) ==
         QuitRequestStart::kNoHandler);
  EXPECT(replies.empty() && !QuitRequestPending());
  EXPECT(!AnswerQuitRequest(true));

  // Asked once with its kind; a second request waits on the first.
  SetQuitRequestedHandler(Asked, nullptr);
  uint64_t first = 0;
  EXPECT(BeginQuitRequest(LAUFEY_QUIT_REQUEST_SESSION_END, record, &first) ==
         QuitRequestStart::kAsked);
  EXPECT(g_asked.size() == 1 && g_asked[0] == LAUFEY_QUIT_REQUEST_SESSION_END);
  EXPECT(QuitRequestPending());
  EXPECT(BeginQuitRequest(LAUFEY_QUIT_REQUEST_APP, record) ==
         QuitRequestStart::kPending);
  EXPECT(g_asked.size() == 1);

  // The first answer wins and replies once.
  EXPECT(AnswerQuitRequest(false));
  EXPECT(!AnswerQuitRequest(true));
  EXPECT(replies.size() == 1 && replies[0] == false);

  // The old deadline does nothing to a newer request; its own deadline allows.
  uint64_t second = 0;
  EXPECT(BeginQuitRequest(LAUFEY_QUIT_REQUEST_APP, record, &second) ==
         QuitRequestStart::kAsked);
  EXPECT(second != first);
  ExpireQuitRequest(first);
  EXPECT(QuitRequestPending());
  ExpireQuitRequest(second);
  EXPECT(!QuitRequestPending());
  EXPECT(replies.size() == 2 && replies[1] == true);

  // A deadline after the answer is a no-op.
  ExpireQuitRequest(second);
  EXPECT(replies.size() == 2);

  // Clearing the handler lets a request in progress go ahead.
  EXPECT(BeginQuitRequest(LAUFEY_QUIT_REQUEST_APP, record) ==
         QuitRequestStart::kAsked);
  SetQuitRequestedHandler(nullptr, nullptr);
  EXPECT(replies.size() == 3 && replies[2] == true);

  // A handler may answer from inside the call without deadlocking.
  SetQuitRequestedHandler(AnswersAtOnce, nullptr);
  EXPECT(BeginQuitRequest(LAUFEY_QUIT_REQUEST_APP, record) ==
         QuitRequestStart::kAsked);
  EXPECT(replies.size() == 4 && replies[3] == false);
  SetQuitRequestedHandler(nullptr, nullptr);
  return 0;
}
