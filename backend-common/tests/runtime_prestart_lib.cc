// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Stand-in runtime libraries for runtime_prestart_test.cc. Built twice: with
// LAUFEY_TEST_PRESTART it exports laufey_runtime_prestart, which records the
// engine it was given in LAUFEY_TEST_PRESTART_SEEN and answers as
// LAUFEY_TEST_PRESTART_EXIT says (unset: go on; a number: exit with it);
// without, it exports only the three entry points every runtime has.

#define LAUFEY_TEST_EXPORT extern "C" __attribute__((visibility("default")))

#include <cstdlib>
#include <string>

LAUFEY_TEST_EXPORT int laufey_runtime_init(const void* /*api*/) {
  return 0;
}

LAUFEY_TEST_EXPORT int laufey_runtime_start() {
  return 0;
}

LAUFEY_TEST_EXPORT void laufey_runtime_shutdown() {}

#ifdef LAUFEY_TEST_PRESTART
LAUFEY_TEST_EXPORT int laufey_runtime_prestart(int engine, int* exit_code) {
  setenv("LAUFEY_TEST_PRESTART_SEEN", std::to_string(engine).c_str(), 1);
  const char* exit_with = std::getenv("LAUFEY_TEST_PRESTART_EXIT");
  if (!exit_with)
    return 1;
  if (exit_code)
    *exit_code = std::atoi(exit_with);
  return 0;
}
#endif
