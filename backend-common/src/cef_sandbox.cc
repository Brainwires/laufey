// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "laufey_cef_sandbox.h"

#include <sys/stat.h>

#if defined(__linux__)
#include <fcntl.h>
#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#endif

namespace laufey_common {

LinuxSandboxDecision DecideLinuxSandbox(const LinuxSandboxFacts& facts) {
  LinuxSandboxDecision decision;
  if (facts.running_as_root) {
    // Chromium exits rather than start sandboxed as root.
    decision.reason =
        "running as root (Chromium does not support its sandbox for root)";
    return decision;
  }
  if (facts.user_namespaces) {
    decision.mode = LinuxSandboxMode::kNamespace;
    return decision;
  }
  if (facts.helper.usable) {
    decision.mode = LinuxSandboxMode::kSetuid;
    return decision;
  }
  std::string reason =
      facts.apparmor_restricts_user_namespaces
          ? "unprivileged user namespaces are restricted by AppArmor "
            "(kernel.apparmor_restrict_unprivileged_userns=1)"
          : "unprivileged user namespaces are not available";
  if (facts.helper.present) {
    reason += ", and the chrome-sandbox helper is not usable (" +
              facts.helper.problem + ")";
  } else {
    reason += ", and there is no chrome-sandbox helper next to the executable";
  }
  reason +=
      "; install the app from its .deb or .rpm package to run web content "
      "sandboxed";
  decision.reason = reason;
  return decision;
}

const char* LinuxSandboxModeName(LinuxSandboxMode mode) {
  switch (mode) {
    case LinuxSandboxMode::kNamespace:
      return "namespace";
    case LinuxSandboxMode::kSetuid:
      return "setuid";
    case LinuxSandboxMode::kOff:
      break;
  }
  return "off";
}

SetuidHelperState InspectSetuidHelper(const std::string& path) {
  SetuidHelperState state;
#if !defined(_WIN32)
  struct stat st;
  if (path.empty() || stat(path.c_str(), &st) != 0) {
    return state;
  }
  state.present = true;
  if (!S_ISREG(st.st_mode)) {
    state.problem = "not a regular file";
  } else if (st.st_uid != 0) {
    state.problem = "not owned by root";
  } else if (!(st.st_mode & S_ISUID)) {
    state.problem = "not setuid";
  } else if (!(st.st_mode & S_IXOTH)) {
    state.problem = "not executable";
  } else {
    state.usable = true;
  }
#else
  (void)path;
#endif
  return state;
}

#if defined(__linux__)

namespace {

// Async-signal-safe write of a short string to a /proc file.
bool WriteProcFile(const char* path, const char* data) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  size_t len = strlen(data);
  ssize_t written = write(fd, data, len);
  close(fd);
  return written == static_cast<ssize_t>(len);
}

// Formats "<id> <id> 1\n" without the allocator or stdio locks (this runs in
// a forked child of a threaded process).
void FormatIdMap(char* out, size_t size, unsigned id) {
  char digits[16];
  int n = 0;
  do {
    digits[n++] = static_cast<char>('0' + id % 10);
    id /= 10;
  } while (id && n < 15);
  size_t pos = 0;
  for (int pass = 0; pass < 2; ++pass) {
    for (int i = n - 1; i >= 0 && pos + 1 < size; --i) {
      out[pos++] = digits[i];
    }
    if (pos + 1 < size) {
      out[pos++] = ' ';
    }
  }
  const char tail[] = "1\n";
  for (size_t i = 0; tail[i] && pos + 1 < size; ++i) {
    out[pos++] = tail[i];
  }
  out[pos] = '\0';
}

}  // namespace

bool CanCreateUserNamespaces() {
  struct stat st;
  if (stat("/proc/self/ns/user", &st) != 0) {
    return false;
  }
  const uid_t uid = getuid();
  const gid_t gid = getgid();
  char uid_map[48];
  char gid_map[48];
  FormatIdMap(uid_map, sizeof(uid_map), static_cast<unsigned>(uid));
  FormatIdMap(gid_map, sizeof(gid_map), static_cast<unsigned>(gid));

  pid_t pid = fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    // What Chromium's probe does: a new user namespace, the caller's ids
    // mapped into it, then a nested one (which AppArmor's
    // unprivileged_userns profile refuses, as some kernels refuse it by
    // sysctl).
    if (unshare(CLONE_NEWUSER) != 0) {
      _exit(1);
    }
    // setgroups must be denied before an unprivileged gid_map write; the
    // file is missing on kernels older than 3.19, where it isn't needed.
    int fd = open("/proc/self/setgroups", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
      const char deny[] = "deny";
      ssize_t ignored = write(fd, deny, sizeof(deny) - 1);
      (void)ignored;
      close(fd);
    }
    if (!WriteProcFile("/proc/self/uid_map", uid_map) ||
        !WriteProcFile("/proc/self/gid_map", gid_map)) {
      _exit(1);
    }
    if (unshare(CLONE_NEWUSER) != 0) {
      _exit(1);
    }
    _exit(0);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      return false;
    }
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

LinuxSandboxFacts ProbeLinuxSandbox(const std::string& exe_dir) {
  LinuxSandboxFacts facts;
  facts.running_as_root = geteuid() == 0;
  if (facts.running_as_root) {
    return facts;
  }
  facts.user_namespaces = CanCreateUserNamespaces();
  if (!facts.user_namespaces) {
    if (FILE* f = std::fopen(
            "/proc/sys/kernel/apparmor_restrict_unprivileged_userns", "re")) {
      facts.apparmor_restricts_user_namespaces = std::fgetc(f) == '1';
      std::fclose(f);
    }
  }
  facts.helper = InspectSetuidHelper(
      exe_dir.empty() ? std::string() : exe_dir + "/chrome-sandbox");
  return facts;
}

#endif  // defined(__linux__)

}  // namespace laufey_common
