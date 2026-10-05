// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Unit tests for the CEF Linux sandbox decision (src/cef_sandbox.cc). No test
// framework; exits non-zero if any expectation fails.

#include "laufey_cef_sandbox.h"

#include <sys/stat.h>

#include <cstdio>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#endif

using namespace laufey_common;

static int g_failures = 0;

#define EXPECT(cond)                                                       \
  do {                                                                     \
    if (!(cond)) {                                                         \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                        \
    }                                                                      \
  } while (0)

static bool Has(const std::string& s, const char* part) {
  return s.find(part) != std::string::npos;
}

static SetuidHelperState Helper(bool present, bool usable,
                                const char* problem = "") {
  SetuidHelperState h;
  h.present = present;
  h.usable = usable;
  h.problem = problem;
  return h;
}

static void TestDecision() {
  LinuxSandboxFacts facts;

  // User namespaces win, with or without a helper (Chromium's order).
  facts.user_namespaces = true;
  EXPECT(DecideLinuxSandbox(facts).mode == LinuxSandboxMode::kNamespace);
  EXPECT(DecideLinuxSandbox(facts).enabled());
  EXPECT(DecideLinuxSandbox(facts).reason.empty());
  facts.helper = Helper(true, true);
  EXPECT(DecideLinuxSandbox(facts).mode == LinuxSandboxMode::kNamespace);
  // A broken helper doesn't matter while namespaces work.
  facts.helper = Helper(true, false, "not owned by root");
  EXPECT(DecideLinuxSandbox(facts).mode == LinuxSandboxMode::kNamespace);

  // Without namespaces: the setuid helper when it is usable ...
  facts.user_namespaces = false;
  facts.helper = Helper(true, true);
  EXPECT(DecideLinuxSandbox(facts).mode == LinuxSandboxMode::kSetuid);
  EXPECT(DecideLinuxSandbox(facts).enabled());

  // ... else off, and the reason names both missing layers.
  facts.apparmor_restricts_user_namespaces = true;
  facts.helper = Helper(true, false, "not owned by root");
  LinuxSandboxDecision off = DecideLinuxSandbox(facts);
  EXPECT(off.mode == LinuxSandboxMode::kOff);
  EXPECT(!off.enabled());
  EXPECT(Has(off.reason, "AppArmor"));
  EXPECT(Has(off.reason, "not owned by root"));
  EXPECT(Has(off.reason, ".deb or .rpm"));

  facts.apparmor_restricts_user_namespaces = false;
  facts.helper = Helper(false, false);
  off = DecideLinuxSandbox(facts);
  EXPECT(off.mode == LinuxSandboxMode::kOff);
  EXPECT(!Has(off.reason, "AppArmor"));
  EXPECT(Has(off.reason, "no chrome-sandbox helper"));

  // Root is always off: Chromium refuses to start sandboxed as root.
  facts.running_as_root = true;
  facts.user_namespaces = true;
  facts.helper = Helper(true, true);
  off = DecideLinuxSandbox(facts);
  EXPECT(off.mode == LinuxSandboxMode::kOff);
  EXPECT(Has(off.reason, "root"));

  EXPECT(std::string(LinuxSandboxModeName(LinuxSandboxMode::kNamespace)) ==
         "namespace");
  EXPECT(std::string(LinuxSandboxModeName(LinuxSandboxMode::kSetuid)) ==
         "setuid");
  EXPECT(std::string(LinuxSandboxModeName(LinuxSandboxMode::kOff)) == "off");
}

static void TestInspectSetuidHelper() {
  EXPECT(!InspectSetuidHelper("").present);
  EXPECT(!InspectSetuidHelper("/nonexistent/laufey/chrome-sandbox").present);
#if !defined(_WIN32)
  // A file the test owns: present, but not root-owned and not setuid.
  char path[] = "/tmp/laufey-cef-sandbox-test-XXXXXX";
  int fd = mkstemp(path);
  EXPECT(fd >= 0);
  if (fd >= 0) {
    close(fd);
    chmod(path, 0755);
    SetuidHelperState state = InspectSetuidHelper(path);
    EXPECT(state.present);
    if (geteuid() != 0) {
      EXPECT(!state.usable);
      EXPECT(state.problem == "not owned by root");
    }
    unlink(path);
  }
  // A directory is no helper.
  SetuidHelperState dir = InspectSetuidHelper("/");
  EXPECT(dir.present);
  EXPECT(!dir.usable);
  EXPECT(dir.problem == "not a regular file");
#endif
}

#if defined(__linux__)
static void TestProbe() {
  // The probe runs and agrees with the decision it feeds; what it finds
  // depends on the machine (CI runners allow user namespaces, Ubuntu 24.04
  // desktops restrict them).
  LinuxSandboxFacts facts = ProbeLinuxSandbox("/nonexistent");
  EXPECT(!facts.helper.present);
  EXPECT(facts.running_as_root == (geteuid() == 0));
  LinuxSandboxDecision decision = DecideLinuxSandbox(facts);
  EXPECT(decision.enabled() ==
         (!facts.running_as_root && facts.user_namespaces));
  std::printf("cef_sandbox_test: this machine: %s%s%s\n",
              LinuxSandboxModeName(decision.mode),
              decision.reason.empty() ? "" : " - ", decision.reason.c_str());
}
#endif

int main() {
  TestDecision();
  TestInspectSetuidHelper();
#if defined(__linux__)
  TestProbe();
#endif
  if (g_failures) {
    std::fprintf(stderr, "%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("cef_sandbox_test: all passed\n");
  return 0;
}
