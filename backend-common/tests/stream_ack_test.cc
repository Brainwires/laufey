// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// ParseStreamAckCount (laufey_stream_ack.h): the count in a stream ack the
// page posts is taken only when it is 1 to 11 ASCII digits; anything else is
// ignored, never parsed into a number or thrown on. Plain asserts, no
// framework.

#include "laufey_stream_ack.h"

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

using laufey_common::ParseStreamAckCount;

static bool Rejects(const std::string& n) {
  size_t out = 77;
  return !ParseStreamAckCount(n, &out) && out == 77;
}

static bool Accepts(const std::string& n, size_t want) {
  size_t out = 77;
  return ParseStreamAckCount(n, &out) && out == want;
}

int main() {
  EXPECT(Rejects("x"));
  EXPECT(Rejects(""));
  EXPECT(Rejects("1e3"));
  EXPECT(Rejects("-1"));
  EXPECT(Rejects("+1"));
  EXPECT(Rejects(" 1"));
  EXPECT(Rejects("1 "));
  EXPECT(Rejects("0x10"));
  EXPECT(Rejects("4.2"));
  EXPECT(Rejects("999999999999"));  // 12 digits
  EXPECT(Rejects(std::string("1\0", 2)));
  EXPECT(Accepts("0", 0));
  EXPECT(Accepts("42", 42));
  EXPECT(Accepts("0042", 42));
  if (sizeof(size_t) >= 8)
    EXPECT(Accepts("99999999999", 99999999999ull));  // 11 digits
  std::printf("stream_ack_test: ok\n");
  return 0;
}
