// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The runtime's prestart call (laufey_runtime_prestart.h): a host calls it
// before its toolkit and web engine. A library that exports the symbol is
// called with the host's engine and decides whether the host goes on; one
// that doesn't, a missing library and no library at all leave the host as
// it was.

#include "laufey_runtime_prestart.h"

#include <cstdio>
#include <cstdlib>
#include <string>

static int failures = 0;

#define CHECK(cond)                                              \
  do {                                                           \
    if (!(cond)) {                                               \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, \
                   __LINE__, #cond);                             \
      ++failures;                                                \
    }                                                            \
  } while (0)

int main() {
  const std::string with = LAUFEY_PRESTART_LIB;
  const std::string without = LAUFEY_PRESTART_CONTROL_LIB;
  int code = -1;

  // No runtime, a library that isn't there, a runtime without the symbol:
  // the host goes on, and the exit code is left alone.
  CHECK(laufey_common::RuntimePrestart("", LAUFEY_ENGINE_WEBVIEW, &code));
  CHECK(laufey_common::RuntimePrestart("/nonexistent/libruntime.so",
                                       LAUFEY_ENGINE_WEBVIEW, &code));
  unsetenv("LAUFEY_TEST_PRESTART_SEEN");
  CHECK(laufey_common::RuntimePrestart(without, LAUFEY_ENGINE_CEF, &code));
  CHECK(std::getenv("LAUFEY_TEST_PRESTART_SEEN") == nullptr);
  CHECK(code == -1);

  // A runtime with the symbol is called with the host's engine and says go.
  unsetenv("LAUFEY_TEST_PRESTART_EXIT");
  CHECK(laufey_common::RuntimePrestart(with, LAUFEY_ENGINE_CEF, &code));
  const char* seen = std::getenv("LAUFEY_TEST_PRESTART_SEEN");
  CHECK(seen && std::string(seen) == "2");
  CHECK(code == -1);
  CHECK(laufey_common::RuntimePrestart(with, LAUFEY_ENGINE_WEBVIEW, &code));
  seen = std::getenv("LAUFEY_TEST_PRESTART_SEEN");
  CHECK(seen && std::string(seen) == "1");

  // It says exit, with its code (zero included).
  setenv("LAUFEY_TEST_PRESTART_EXIT", "7", 1);
  CHECK(!laufey_common::RuntimePrestart(with, LAUFEY_ENGINE_WEBVIEW, &code));
  CHECK(code == 7);
  setenv("LAUFEY_TEST_PRESTART_EXIT", "0", 1);
  CHECK(!laufey_common::RuntimePrestart(with, LAUFEY_ENGINE_WEBVIEW, &code));
  CHECK(code == 0);
  // A null exit_code is tolerated.
  CHECK(!laufey_common::RuntimePrestart(with, LAUFEY_ENGINE_WEBVIEW, nullptr));

  if (failures) {
    std::fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("runtime_prestart_test: ok");
  return 0;
}
