// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The CEF profile's LevelDB LOCK files and the helper processes
// (laufey_cef_profile_lock.h).

#include "laufey_cef_profile_lock.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

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
#include <iterator>
#endif

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
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

// CRC-32C (Castagnoli), as LevelDB's log records carry it.
uint32_t Crc32c(const unsigned char* data, size_t n) {
  static const auto table = [] {
    std::vector<uint32_t> t(256);
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k)
        c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i)
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return crc ^ 0xFFFFFFFFu;
}

uint32_t UnmaskCrc(uint32_t masked) {
  uint32_t rot = masked - 0xA282EAD8u;
  return (rot >> 17) | (rot << 15);
}

std::filesystem::path FsPath(const std::string& utf8) {
#if defined(_WIN32)
  return std::filesystem::path(Utf8ToWide(utf8));
#else
  return std::filesystem::path(utf8);
#endif
}

std::string Utf8Of(const std::filesystem::path& path) {
#if defined(_WIN32)
  return WideToUtf8(path.wstring());
#else
  return path.string();
#endif
}

int64_t RepairLevelDbLogTailAt(const std::filesystem::path& path);

}  // namespace

int64_t LevelDbLogKeepLength(const std::vector<unsigned char>& log) {
  // leveldb/db/log_format.h: 32 KiB blocks of records, each a header
  // (masked CRC-32C of type + payload, payload length, type) and a payload;
  // a block's last < 7 bytes are a zero trailer. A logical record is one
  // FULL record or FIRST, MIDDLE..., LAST.
  constexpr size_t kBlock = 32768;
  constexpr size_t kHeader = 7;
  enum { kZero = 0, kFirst = 2, kMiddle = 3 };
  const size_t n = log.size();
  size_t off = 0;
  size_t logical_start = 0;
  bool in_fragments = false;
  while (off < n) {
    size_t left = kBlock - off % kBlock;
    if (left < kHeader) {  // the trailer
      off += left;
      continue;
    }
    size_t start = in_fragments ? logical_start : off;
    if (n - off < kHeader)  // a header cut short by the end of the file
      return static_cast<int64_t>(start);
    const unsigned char* h = log.data() + off;
    uint32_t crc = static_cast<uint32_t>(h[0]) | (h[1] << 8) | (h[2] << 16) |
                   (static_cast<uint32_t>(h[3]) << 24);
    size_t length =
        static_cast<size_t>(h[4]) | (static_cast<size_t>(h[5]) << 8);
    unsigned type = h[6];
    // As leveldb::log::Reader::ReadPhysicalRecord.
    const bool final_block = n - off < left;  // the file ends in this block
    if (kHeader + length > (final_block ? n - off : left)) {
      // At the end of the file: the payload was cut short. Elsewhere a bad
      // length, which drops the rest of the block (below).
      if (final_block)
        return static_cast<int64_t>(start);
    } else if (type == kZero && length == 0) {  // preallocated: skipped
      off += left;
      continue;
    } else if (UnmaskCrc(crc) == Crc32c(h + 6, 1 + length)) {
      if (type == kFirst) {
        logical_start = off;
        in_fragments = true;
      } else if (type != kMiddle) {  // FULL, LAST, or an unknown type
        in_fragments = false;
      }
      off += kHeader + length;
      continue;
    }
    // A record that doesn't check: LevelDB drops the rest of its block (and
    // a logical record it was part of). In the file's last block that is
    // everything after it, and the next write would follow it there and be
    // dropped with it on the next open; in an earlier block the blocks after
    // it still read, so go on with them.
    if (off / kBlock == (n - 1) / kBlock)
      return static_cast<int64_t>(start);
    off += left;
    in_fragments = false;
  }
  // A FIRST without its LAST at the end: the write was cut short.
  return in_fragments ? static_cast<int64_t>(logical_start) : -1;
}

int64_t RepairLevelDbLogTail(const std::string& log_file) {
  return RepairLevelDbLogTailAt(FsPath(log_file));
}

namespace {

int64_t RepairLevelDbLogTailAt(const std::filesystem::path& path) {
  std::error_code ec;
  uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec || size == 0 || size > (64u << 20))
    return 0;
  std::vector<unsigned char> data(static_cast<size_t>(size));
  {
    std::ifstream in(path, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(data.data()),
                 static_cast<std::streamsize>(data.size())))
      return 0;
  }
  int64_t keep = LevelDbLogKeepLength(data);
  if (keep < 0)
    return 0;
  std::filesystem::resize_file(path, static_cast<uintmax_t>(keep), ec);
  if (ec)
    return 0;
  return static_cast<int64_t>(size) - keep;
}

}  // namespace

int RepairProfileLevelDbLogs(const std::string& root_cache_dir) {
  int repaired = 0;
  for (const std::string& lock : ProfileLevelDbLockFiles(root_cache_dir)) {
    std::error_code ec;
    std::filesystem::path dir = FsPath(lock).parent_path();
    // Only a database no one has open (its LOCK free).
    if (!std::filesystem::is_directory(dir, ec) || LevelDbLockHeld(lock))
      continue;
    for (std::filesystem::directory_iterator it(dir, ec), end;
         !ec && it != end; it.increment(ec)) {
      const std::filesystem::directory_entry& entry = *it;
      const std::string name = Utf8Of(entry.path().filename());
      if (name.size() < 5 || name.compare(name.size() - 4, 4, ".log") != 0 ||
          name.find_first_not_of("0123456789") != name.size() - 4)
        continue;
      int64_t removed = RepairLevelDbLogTailAt(entry.path());
      if (removed > 0) {
        ++repaired;
        std::cerr << "laufey: cut " << removed
                  << " bytes of an interrupted write from the end of "
                  << Utf8Of(entry.path())
                  << " (what is written next would otherwise be lost)"
                  << std::endl;
      }
    }
  }
  return repaired;
}

std::string CefHelperType(const std::string& command_line) {
  size_t i = 0;
  const size_t n = command_line.size();
  auto separator = [](char c) {
    return c == '\0' || c == ' ' || c == '\t' || c == '"';
  };
  while (i < n) {
    while (i < n && separator(command_line[i]))
      ++i;
    size_t start = i;
    while (i < n && !separator(command_line[i]))
      ++i;
    if (command_line.compare(start, 7, "--type=") == 0 && i - start > 7)
      return command_line.substr(start + 7, i - start - 7);
  }
  return std::string();
}

bool IsCefHelperToWaitFor(const std::string& command_line) {
  bool helper = false;
  size_t i = 0;
  const size_t n = command_line.size();
  auto separator = [](char c) {
    return c == '\0' || c == ' ' || c == '\t' || c == '"';
  };
  while (i < n) {
    while (i < n && separator(command_line[i]))
      ++i;
    size_t start = i;
    while (i < n && !separator(command_line[i]))
      ++i;
    std::string arg = command_line.substr(start, i - start);
    if (arg.rfind("--type=", 0) != 0)
      continue;
    std::string type = arg.substr(7);
    // Chromium's zygotes (Linux) hold the browser's IPC socket and the
    // crash handler watches it: both end only after the browser process.
    // On macOS the browser starts code-sign-clone-cleanup during shutdown
    // to delete its code-signing clone of the app bundle once the browser
    // has exited: it polls getppid() until it is reparented, so waiting for
    // it only ever ends at the deadline (chrome/browser/mac/
    // code_sign_clone_manager.mm).
    if (type == "zygote" || type == "crashpad-handler" ||
        type == "code-sign-clone-cleanup")
      return false;
    helper = true;
  }
  return helper;
}

namespace {

#if defined(_WIN32)
// The command line of the process behind `process` (opened with
// PROCESS_QUERY_LIMITED_INFORMATION), "" if it can't be read.
std::string ProcessCommandLine(HANDLE process) {
  using NtQueryInformationProcessFn =
      LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  static const auto query = reinterpret_cast<NtQueryInformationProcessFn>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"),
                     "NtQueryInformationProcess"));
  if (!query)
    return "";
  struct CountedString {  // UNICODE_STRING
    USHORT length;
    USHORT maximum_length;
    PWSTR buffer;
  };
  constexpr ULONG kProcessCommandLineInformation = 60;  // Windows 8.1+
  ULONG size = 0;
  query(process, kProcessCommandLineInformation, nullptr, 0, &size);
  if (size < sizeof(CountedString) || size > (1u << 20))
    return "";
  std::vector<unsigned char> buf(size);
  if (query(process, kProcessCommandLineInformation, buf.data(), size,
            &size) < 0)
    return "";
  const auto* line = reinterpret_cast<const CountedString*>(buf.data());
  if (!line->buffer)
    return "";
  return WideToUtf8(
      std::wstring(line->buffer, line->length / sizeof(wchar_t)));
}
#elif defined(__APPLE__)
// A process's arguments, NUL-separated (KERN_PROCARGS2), "" if they can't
// be read.
std::string ProcessCommandLine(pid_t pid) {
  int argmax = 0;
  size_t len = sizeof(argmax);
  int argmax_mib[2] = {CTL_KERN, KERN_ARGMAX};
  if (sysctl(argmax_mib, 2, &argmax, &len, nullptr, 0) != 0 || argmax <= 0)
    return "";
  std::vector<char> buf(static_cast<size_t>(argmax));
  size_t size = buf.size();
  int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
  if (sysctl(mib, 3, buf.data(), &size, nullptr, 0) != 0 ||
      size < sizeof(int))
    return "";
  int argc = 0;
  std::memcpy(&argc, buf.data(), sizeof(argc));
  size_t i = sizeof(int);
  while (i < size && buf[i] != '\0')  // the executable path
    ++i;
  while (i < size && buf[i] == '\0')  // its padding
    ++i;
  std::string out;
  for (int a = 0; a < argc && i < size; ++a) {
    size_t start = i;
    while (i < size && buf[i] != '\0')
      ++i;
    out.append(buf.data() + start, i - start);
    out.push_back('\0');
    ++i;
  }
  return out;
}
#endif

// This process's running (not zombie) children whose command line `match`
// accepts.
int RunningChildren(const std::function<bool(const std::string&)>& match) {
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
        HANDLE p =
            OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                        FALSE, entry.th32ProcessID);
        if (p) {
          if (WaitForSingleObject(p, 0) == WAIT_TIMEOUT &&
              match(ProcessCommandLine(p)))
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
        info.pbsi_status != SZOMB &&
        match(ProcessCommandLine(pids[i])))
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
    if (ppid != self || state == 'Z' || state == 'X')
      continue;
    std::ifstream cmd_file(std::string("/proc/") + e->d_name + "/cmdline",
                           std::ios::binary);
    std::string cmdline((std::istreambuf_iterator<char>(cmd_file)),
                        std::istreambuf_iterator<char>());
    if (match(cmdline))
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
  int left = RunningChildren(IsCefHelperToWaitFor);
  while (left > 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    left = RunningChildren(IsCefHelperToWaitFor);
  }
  return left;
}

int WaitForCefHelpersOfType(const std::string& type, int timeout_ms,
                            const std::function<void()>& pump) {
  auto match = [&type](const std::string& command_line) {
    return CefHelperType(command_line) == type;
  };
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  int left = RunningChildren(match);
  while (left > 0 && std::chrono::steady_clock::now() < deadline) {
    if (pump)
      pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    left = RunningChildren(match);
  }
  return left;
}

void LaufeyWaitForRenderersBeforeShutdown(const std::function<void()>& pump) {
  int left = WaitForCefHelpersOfType("renderer", 2000, pump);
  if (left > 0) {
    std::cerr << "laufey: " << left
              << " renderer process(es) still running 2 s after the loop "
                 "ended; localStorage written just before may be lost"
              << std::endl;
  }
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
  RepairProfileLevelDbLogs(root_cache_dir);
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
