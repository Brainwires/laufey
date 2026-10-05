// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The Chromium sandbox of the CEF backend on Linux: whether the host can turn
// it on, and with which layer-1 sandbox. See docs/backends.md, "The Chromium
// sandbox".
//
// Chromium's renderer, GPU and utility processes run under seccomp-bpf plus a
// layer-1 sandbox that puts them in their own namespaces. Layer 1 is either
// the namespace sandbox (unprivileged user namespaces) or the setuid helper
// `chrome-sandbox` next to the executable (root-owned, mode 4755, which only a
// system package install can arrange). With neither, Chromium aborts at start
// ("No usable sandbox!"), and as root it refuses to start sandboxed at all.
// Ubuntu 23.10 and later restrict unprivileged user namespaces with AppArmor
// (kernel.apparmor_restrict_unprivileged_userns), so a tarball or AppImage run
// by the user there has neither.
//
// The host therefore probes what Chromium would find, the same way Chromium
// does, before CefInitialize: with a usable sandbox it runs sandboxed, and
// without one it turns the sandbox off and says why, instead of failing to
// start. The decision depends only on the machine and the installed files,
// never on the command line, so a deep link can't influence it (Chromium's own
// --no-sandbox switch is one of the switches a deep-link launch drops; see
// laufey_launch_args.h).

#ifndef LAUFEY_CEF_SANDBOX_H_
#define LAUFEY_CEF_SANDBOX_H_

#include <string>

namespace laufey_common {

// The setuid sandbox helper (`chrome-sandbox` next to the executable).
struct SetuidHelperState {
  bool present = false;
  // Owned by root, setuid, executable: what Chromium requires before it uses
  // the helper. A helper that is present but not usable makes Chromium abort
  // when it has to fall back to it, so it counts as absent.
  bool usable = false;
  // Why a present helper is not usable ("" when usable or absent).
  std::string problem;
};

// What the host learned about the machine.
struct LinuxSandboxFacts {
  bool running_as_root = false;
  // A child could create a user namespace, map its ids and create a nested
  // one: Chromium's CanCreateProcessInNewUserNS.
  bool user_namespaces = false;
  // kernel.apparmor_restrict_unprivileged_userns is 1 (only used to explain a
  // missing namespace sandbox).
  bool apparmor_restricts_user_namespaces = false;
  SetuidHelperState helper;
};

enum class LinuxSandboxMode {
  kNamespace,  // unprivileged user namespaces
  kSetuid,     // the chrome-sandbox helper
  kOff,        // CefSettings::no_sandbox
};

struct LinuxSandboxDecision {
  LinuxSandboxMode mode = LinuxSandboxMode::kOff;
  // Why the sandbox is off ("" when it is on). Printed as a warning.
  std::string reason;

  bool enabled() const {
    return mode != LinuxSandboxMode::kOff;
  }
};

// Chromium's choice, made ahead of it: the namespace sandbox when user
// namespaces work, else the setuid helper when it is usable, else off. Root
// is always off.
LinuxSandboxDecision DecideLinuxSandbox(const LinuxSandboxFacts& facts);

// "namespace", "setuid" or "off".
const char* LinuxSandboxModeName(LinuxSandboxMode mode);

// Checks `path` as Chromium checks its setuid helper.
SetuidHelperState InspectSetuidHelper(const std::string& path);

#if defined(__linux__)
// Forks a child that tries what Chromium's CanCreateProcessInNewUserNS
// tries. Safe to call from a threaded process: the child only makes system
// calls and exits.
bool CanCreateUserNamespaces();

// Probes the running machine; `exe_dir` is the directory of the executable
// (where Chromium looks for chrome-sandbox).
LinuxSandboxFacts ProbeLinuxSandbox(const std::string& exe_dir);
#endif

}  // namespace laufey_common

#endif  // LAUFEY_CEF_SANDBOX_H_
