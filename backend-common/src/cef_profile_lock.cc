// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The CEF profile's LevelDB LOCK files and the helper processes
// (laufey_cef_profile_lock.h).

#include "laufey_cef_profile_lock.h"

#include <chrono>
#include <iostream>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#endif

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc.h>
#endif

#include "laufey_backend_common.h"

namespace laufey_common {

std::vector<std::string> ProfileLevelDbLockFiles(
    const std::string& root_cache_dir) {
  std::vector<std::string> out;
  if (root_cache_dir.empty())
    return out;
#if defined(_WIN32)
  const char sep = '\\';
#else
  const char sep = '/';
#endif
  std::string root = root_cache_dir;
  while (root.size() > 1 && (root.back() == '/' || root.back() == '\\'))
    root.pop_back();
  for (const std::string& profile : {root + sep + "Default", root}) {
    out.push_back(profile + sep + "Local Storage" + sep + "leveldb" + sep +
                  "LOCK");
    out.push_back(profile + sep + "Session Storage" + sep + "LOCK");
  }
  return out;
}

bool LevelDbLockHeld(const std::string& lock_file) {
#if defined(_WIN32)
  std::wstring path = Utf8ToWide(lock_file);
  HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                         nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    // Missing: free. A sharing violation: someone holds it.
    return err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION;
  }
  OVERLAPPED ov = {};
  bool held = !LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                          0, MAXDWORD, MAXDWORD, &ov);
  if (!held)
    UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov);
  CloseHandle(h);
  return held;
#else
  int fd = open(lock_file.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0)
    return false;  // missing (or unreadable: nothing to wait for)
  struct flock fl;
  std::memset(&fl, 0, sizeof(fl));
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  bool held = false;
  if (fcntl(fd, F_SETLK, &fl) == -1) {
    held = errno == EACCES || errno == EAGAIN;
  } else {
    fl.l_type = F_UNLCK;
    fcntl(fd, F_SETLK, &fl);
  }
  close(fd);
  return held;
#endif
}

int WaitForProfileLevelDbLocks(const std::string& root_cache_dir,
                               int timeout_ms,
                               std::vector<std::string>* still_held) {
  std::vector<std::string> files = ProfileLevelDbLockFiles(root_cache_dir);
  auto start = std::chrono::steady_clock::now();
  auto deadline = start + std::chrono::milliseconds(timeout_ms);
  std::vector<std::string> held;
  for (;;) {
    held.clear();
    for (const std::string& f : files) {
      if (LevelDbLockHeld(f))
        held.push_back(f);
    }
    if (held.empty() || std::chrono::steady_clock::now() >= deadline)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  if (still_held)
    *still_held = held;
  return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - start)
                              .count());
}

namespace {

// This process's running (not zombie) children.
int RunningChildren() {
#if defined(_WIN32)
  DWORD self = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return 0;
  PROCESSENTRY32W entry;
  entry.dwSize = sizeof(entry);
  int n = 0;
  if (Process32FirstW(snap, &entry)) {
    do {
      if (entry.th32ParentProcessID == self && entry.th32ProcessID != self) {
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, entry.th32ProcessID);
        if (p) {
          if (WaitForSingleObject(p, 0) == WAIT_TIMEOUT)
            ++n;
          CloseHandle(p);
        }
      }
    } while (Process32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return n;
#elif defined(__APPLE__)
  pid_t pids[512];
  // libproc answers with the number of pids (not bytes).
  int count = proc_listchildpids(getpid(), pids, sizeof(pids));
  if (count <= 0)
    return 0;
  if (count > 512)
    count = 512;
  int n = 0;
  for (int i = 0; i < count; ++i) {
    struct proc_bsdshortinfo info;
    if (proc_pidinfo(pids[i], PROC_PIDT_SHORTBSDINFO, 0, &info,
                     sizeof(info)) == sizeof(info) &&
        info.pbsi_status != SZOMB)
      ++n;
  }
  return n;
#else
  pid_t self = getpid();
  DIR* proc = opendir("/proc");
  if (!proc)
    return 0;
  int n = 0;
  while (struct dirent* e = readdir(proc)) {
    if (e->d_name[0] < '0' || e->d_name[0] > '9')
      continue;
    std::ifstream stat(std::string("/proc/") + e->d_name + "/stat");
    std::string line;
    if (!std::getline(stat, line))
      continue;
    size_t close_paren = line.rfind(')');
    if (close_paren == std::string::npos || close_paren + 4 > line.size())
      continue;
    char state = line[close_paren + 2];
    long ppid = std::strtol(line.c_str() + close_paren + 4, nullptr, 10);
    if (ppid == self && state != 'Z' && state != 'X')
      ++n;
  }
  closedir(proc);
  return n;
#endif
}

}  // namespace

int WaitForChildProcessesToExit(int timeout_ms) {
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  int left = RunningChildren();
  while (left > 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    left = RunningChildren();
  }
  return left;
}

void LaufeyWaitForProfileLocksBeforeInit(const std::string& root_cache_dir) {
  std::vector<std::string> held;
  int waited = WaitForProfileLevelDbLocks(root_cache_dir, 5000, &held);
  if (!held.empty()) {
    std::cerr << "laufey: the profile's storage is still locked by another "
                 "process after "
              << waited << " ms (" << held.front()
              << "); its localStorage may not persist this launch" << std::endl;
  } else if (waited >= 50) {
    std::cerr << "laufey: waited " << waited
              << " ms for the previous launch to release the profile's "
                 "storage"
              << std::endl;
  }
}

void LaufeyWaitForHelpersAfterShutdown(const std::string& root_cache_dir) {
  int left = WaitForChildProcessesToExit(5000);
  if (left > 0) {
    std::cerr << "laufey: " << left
              << " helper process(es) still running 5 s after shutdown"
              << std::endl;
  }
  // Whatever still holds the profile's storage (a helper that isn't a
  // child: Chromium's zygote re-parents its renderers on Linux).
  std::vector<std::string> held;
  WaitForProfileLevelDbLocks(root_cache_dir, left > 0 ? 1000 : 3000, &held);
}

}  // namespace laufey_common
