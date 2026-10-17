// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_cef_profile_lock.h: the profile's LevelDB LOCK files, the probe
// against a lock another process holds, the bounded wait, the repair of torn
// log ends (localStorage, sessionStorage, IndexedDB), and the wait for child
// processes.

#include "laufey_cef_profile_lock.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#endif

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using namespace laufey_common;

namespace {

// A LevelDB log writer (leveldb/db/log_writer.cc), for the repair tests.
uint32_t Crc32c(const unsigned char* p, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k)
      crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
  }
  return crc ^ 0xFFFFFFFFu;
}

void EmitPhysical(std::vector<unsigned char>* log, unsigned type,
                  const unsigned char* p, size_t n) {
  std::vector<unsigned char> body(1 + n);
  body[0] = static_cast<unsigned char>(type);
  std::memcpy(body.data() + 1, p, n);
  uint32_t crc = Crc32c(body.data(), body.size());
  crc = ((crc >> 15) | (crc << 17)) + 0xA282EAD8u;  // masked
  unsigned char h[7] = {
      static_cast<unsigned char>(crc), static_cast<unsigned char>(crc >> 8),
      static_cast<unsigned char>(crc >> 16),
      static_cast<unsigned char>(crc >> 24), static_cast<unsigned char>(n),
      static_cast<unsigned char>(n >> 8), static_cast<unsigned char>(type)};
  log->insert(log->end(), h, h + 7);
  log->insert(log->end(), p, p + n);
}

// Appends one logical record of `n` bytes, fragmented across blocks.
void AddRecord(std::vector<unsigned char>* log, size_t n, unsigned char fill) {
  std::vector<unsigned char> data(n, fill);
  size_t done = 0;
  bool begin = true;
  do {
    size_t left = 32768 - log->size() % 32768;
    if (left < 7) {
      log->insert(log->end(), left, 0);  // the trailer
      left = 32768;
    }
    size_t avail = left - 7;
    size_t frag = (std::min)(n - done, avail);
    bool end = done + frag == n;
    unsigned type = begin && end ? 1 : begin ? 2 : end ? 4 : 3;
    EmitPhysical(log, type, data.data() + done, frag);
    done += frag;
    begin = false;
  } while (done < n);
}

std::vector<unsigned char> Cut(std::vector<unsigned char> log, size_t n) {
  log.resize(n);
  return log;
}

}  // namespace

int main() {
#if defined(_WIN32)
  auto files = ProfileLevelDbLockFiles("C:\\p\\CEF\\");
  EXPECT(files.size() == 4);
  EXPECT(files[0] == "C:\\p\\CEF\\Default\\Local Storage\\leveldb\\LOCK");
  EXPECT(files[3] == "C:\\p\\CEF\\Session Storage\\LOCK");
#else
  auto files = ProfileLevelDbLockFiles("/p/CEF/");
  EXPECT(files.size() == 4);
  EXPECT(files[0] == "/p/CEF/Default/Local Storage/leveldb/LOCK");
  EXPECT(files[1] == "/p/CEF/Default/Session Storage/LOCK");
  EXPECT(files[2] == "/p/CEF/Local Storage/leveldb/LOCK");
#endif
  EXPECT(ProfileLevelDbLockFiles("").empty());

  // Which children are helpers to wait for, by command line.
  using std::string;
  EXPECT(IsCefHelperToWaitFor(string("/app/x\0--type=utility\0", 22)));
  EXPECT(IsCefHelperToWaitFor(
      "\"C:\\App\\app.exe\" --type=renderer --lang=en-US"));
  EXPECT(IsCefHelperToWaitFor("app --type=gpu-process"));
  EXPECT(!IsCefHelperToWaitFor(string("/app/x\0--type=zygote\0", 21)));
  EXPECT(!IsCefHelperToWaitFor("app.exe --type=crashpad-handler --db=x"));
  // macOS: started at shutdown, it waits for the browser process to exit.
  static const char kCloneCleanup[] =
      "/a/laufey Helper\0--type=code-sign-clone-cleanup\0"
      "--unique-temp-dir-suffix=Ab3dE6\0";
  EXPECT(!IsCefHelperToWaitFor(
      string(kCloneCleanup, sizeof(kCloneCleanup) - 1)));
  EXPECT(!IsCefHelperToWaitFor("deno run sidecar.ts"));  // the app's own
  EXPECT(!IsCefHelperToWaitFor("app.exe --flag --typex=renderer"));
  EXPECT(!IsCefHelperToWaitFor(""));
  EXPECT(!LevelDbLockHeld("/nonexistent/laufey/LOCK"));

  // A child's CEF type, by its --type= switch.
  EXPECT(CefHelperType(string("/a/x\0--type=renderer\0--lang=en\0", 31)) ==
         "renderer");
  EXPECT(CefHelperType("\"C:\\App\\app.exe\" --type=utility --x") ==
         "utility");
  EXPECT(CefHelperType("deno run sidecar.ts").empty());
  EXPECT(CefHelperType("app --typex=renderer").empty());
  EXPECT(CefHelperType("app --type=").empty());
  EXPECT(CefHelperType("").empty());

  // LevelDB log tails. Whole logs are kept as they are.
  std::vector<unsigned char> log;
  EXPECT(LevelDbLogKeepLength(log) == -1);
  AddRecord(&log, 200, 'a');
  AddRecord(&log, 30, 'b');
  const size_t whole = log.size();
  EXPECT(LevelDbLogKeepLength(log) == -1);
  // A write cut short at the end (its payload, its header): cut to before it.
  std::vector<unsigned char> torn = log;
  AddRecord(&torn, 40, 'c');
  EXPECT(LevelDbLogKeepLength(Cut(torn, whole + 7 + 25)) ==
         static_cast<int64_t>(whole));
  EXPECT(LevelDbLogKeepLength(Cut(torn, whole + 3)) ==
         static_cast<int64_t>(whole));
  // A later launch's write appended after the cut-short one (LevelDB reuses
  // the log): it no longer checks, and LevelDB would drop it on the next
  // open; cut to before the damage, after which an append reads again.
  std::vector<unsigned char> poisoned = Cut(torn, whole + 7 + 25);
  AddRecord(&poisoned, 50, 'd');
  EXPECT(LevelDbLogKeepLength(poisoned) == static_cast<int64_t>(whole));
  std::vector<unsigned char> healed = Cut(poisoned, whole);
  AddRecord(&healed, 50, 'd');
  EXPECT(LevelDbLogKeepLength(healed) == -1);
  // A record fragmented over blocks, cut before its LAST: cut to its FIRST.
  std::vector<unsigned char> big = log;
  AddRecord(&big, 70000, 'e');
  EXPECT(LevelDbLogKeepLength(big) == -1);
  EXPECT(big.size() > 65536);
  EXPECT(LevelDbLogKeepLength(Cut(big, 65536 + 100)) ==
         static_cast<int64_t>(whole));
  // Damage in an earlier block, whole records after it: left as it is.
  std::vector<unsigned char> early = log;
  early[10] ^= 0xFF;
  AddRecord(&early, 40000, 'f');
  AddRecord(&early, 10, 'g');
  EXPECT(early.size() > 32768);
  EXPECT(LevelDbLogKeepLength(early) == -1);
  // ...and a torn end after it is still found.
  const size_t early_whole = early.size();
  std::vector<unsigned char> early_torn = early;
  AddRecord(&early_torn, 40, 'h');
  EXPECT(LevelDbLogKeepLength(Cut(early_torn, early_whole + 9)) ==
         static_cast<int64_t>(early_whole));
  // ...but in the last block, cut to it.
  std::vector<unsigned char> late = log;
  late[whole - 5] ^= 0xFF;  // in the second record's payload
  EXPECT(LevelDbLogKeepLength(late) == static_cast<int64_t>(whole - 7 - 30));

#if !defined(_WIN32)
  char tmpl[] = "/tmp/laufey-lock-test-XXXXXX";
  EXPECT(mkdtemp(tmpl) != nullptr);
  std::string root = tmpl;
  std::string dir = root + "/Default/Local Storage/leveldb";
  EXPECT(mkdir((root + "/Default").c_str(), 0700) == 0);
  EXPECT(mkdir((root + "/Default/Local Storage").c_str(), 0700) == 0);
  EXPECT(mkdir(dir.c_str(), 0700) == 0);
  std::string lock = dir + "/LOCK";
  int fd = open(lock.c_str(), O_RDWR | O_CREAT, 0600);
  EXPECT(fd >= 0);
  close(fd);
  EXPECT(!LevelDbLockHeld(lock));

  // The profile's logs: a torn end is cut; a whole log, a log that is not
  // the database's (no <number>.log name) and a missing file are left.
  auto write_file = [](const std::string& path,
                       const std::vector<unsigned char>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
  };
  auto file_size = [](const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? static_cast<int64_t>(st.st_size)
                                        : int64_t{-1};
  };
  const std::string db_log = dir + "/000003.log";
  write_file(db_log, poisoned);
  write_file(dir + "/LOG", poisoned);
  EXPECT(RepairProfileLevelDbLogs(root) == 1);
  EXPECT(file_size(db_log) == static_cast<int64_t>(whole));
  EXPECT(file_size(dir + "/LOG") == static_cast<int64_t>(poisoned.size()));
  EXPECT(RepairProfileLevelDbLogs(root) == 0);  // whole now
  EXPECT(RepairLevelDbLogTail(dir + "/000099.log") == 0);  // missing
  write_file(db_log, poisoned);  // torn again, for the held LOCK below

  // Another process holds it for about a second, as a previous launch's
  // storage service would.
  int ready[2];
  EXPECT(pipe(ready) == 0);
  pid_t child = fork();
  EXPECT(child >= 0);
  if (child == 0) {
    int cfd = open(lock.c_str(), O_RDWR);
    struct flock fl;
    std::memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (cfd < 0 || fcntl(cfd, F_SETLK, &fl) != 0)
      _exit(2);
    char c = 1;
    (void)!write(ready[1], &c, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    _exit(0);
  }
  char c;
  EXPECT(read(ready[0], &c, 1) == 1);
  EXPECT(LevelDbLockHeld(lock));
  // A database someone has open is not touched.
  EXPECT(RepairProfileLevelDbLogs(root) == 0);
  EXPECT(file_size(db_log) == static_cast<int64_t>(poisoned.size()));
  // Too short a wait: still held.
  std::vector<std::string> held;
  WaitForProfileLevelDbLocks(root, 100, &held);
  EXPECT(held.size() == 1 && held[0] == lock);
  // Long enough: free once the holder exits, and it says it waited.
  int waited = WaitForProfileLevelDbLocks(root, 5000, &held);
  EXPECT(held.empty());
  EXPECT(waited >= 50);
  int status = 0;
  waitpid(child, &status, 0);
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  EXPECT(RepairProfileLevelDbLogs(root) == 1);  // free again: cut

  // IndexedDB's databases: an origin's default bucket under IndexedDB/, the
  // other buckets under WebStorage/<id>/IndexedDB/. Their logs are cut as
  // localStorage's are, except a database whose LOCK is held; a directory
  // that isn't a database (the blob store) is left alone.
  EXPECT(ProfileIndexedDbDirs("").empty());
  EXPECT(ProfileIndexedDbDirs(root + "/missing").empty());
  EXPECT(ProfileIndexedDbDirs(root).empty());
  const std::string idb = root + "/Default/IndexedDB";
  const std::string origin_db = idb + "/http_127.0.0.1_8000.indexeddb.leveldb";
  const std::string blob_dir = idb + "/http_127.0.0.1_8000.indexeddb.blob";
  const std::string bucket_db =
      root + "/Default/WebStorage/7/IndexedDB/indexeddb.leveldb";
  for (const std::string& d :
       {idb, origin_db, blob_dir, root + "/Default/WebStorage",
        root + "/Default/WebStorage/7", root + "/Default/WebStorage/7/IndexedDB",
        bucket_db})
    EXPECT(mkdir(d.c_str(), 0700) == 0);
  auto dirs = ProfileIndexedDbDirs(root + "/");
  EXPECT(dirs.size() == 2);
  EXPECT(dirs[0] == origin_db && dirs[1] == bucket_db);
  for (const std::string& d : {origin_db, bucket_db, blob_dir}) {
    int lfd = open((d + "/LOCK").c_str(), O_RDWR | O_CREAT, 0600);
    EXPECT(lfd >= 0);
    close(lfd);
    write_file(d + "/000005.log", poisoned);
  }
  EXPECT(RepairProfileLevelDbLogs(root) == 2);
  EXPECT(file_size(origin_db + "/000005.log") == static_cast<int64_t>(whole));
  EXPECT(file_size(bucket_db + "/000005.log") == static_cast<int64_t>(whole));
  EXPECT(file_size(blob_dir + "/000005.log") ==
         static_cast<int64_t>(poisoned.size()));
  EXPECT(RepairProfileLevelDbLogs(root) == 0);
  // One held by another process (a previous launch's storage service) is
  // not touched; the other still is.
  write_file(origin_db + "/000005.log", poisoned);
  write_file(bucket_db + "/000005.log", poisoned);
  int idb_ready[2];
  int idb_release[2];
  EXPECT(pipe(idb_ready) == 0 && pipe(idb_release) == 0);
  pid_t idb_holder = fork();
  EXPECT(idb_holder >= 0);
  if (idb_holder == 0) {
    int cfd = open((origin_db + "/LOCK").c_str(), O_RDWR);
    struct flock fl;
    std::memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    if (cfd < 0 || fcntl(cfd, F_SETLK, &fl) != 0)
      _exit(2);
    char go = 1;
    (void)!write(idb_ready[1], &go, 1);
    (void)!read(idb_release[0], &go, 1);  // until the parent is done
    _exit(0);
  }
  EXPECT(read(idb_ready[0], &c, 1) == 1);
  EXPECT(RepairProfileLevelDbLogs(root) == 1);
  EXPECT(file_size(origin_db + "/000005.log") ==
         static_cast<int64_t>(poisoned.size()));
  EXPECT(file_size(bucket_db + "/000005.log") == static_cast<int64_t>(whole));
  (void)!write(idb_release[1], &c, 1);
  waitpid(idb_holder, &status, 0);
  EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  EXPECT(RepairProfileLevelDbLogs(root) == 1);  // released: cut now
  EXPECT(file_size(origin_db + "/000005.log") == static_cast<int64_t>(whole));

  // Child processes: a running one is waited for; an ended one isn't.
  // A child is a helper by its --type= switch (sh's $1 here); a zygote, or
  // a process of the app's own (no --type=), is not waited for: a zygote
  // (and macOS's code-sign-clone-cleanup) ends only after the browser
  // process does.
  auto spawn = [](const char* type_arg, const char* secs) {
    pid_t pid = fork();
    if (pid == 0) {
      // A second command keeps sh (and its $1) from exec'ing into sleep.
      std::string script = std::string("sleep ") + secs + "; exit 0";
      if (type_arg)
        execl("/bin/sh", "sh", "-c", script.c_str(), "sh", type_arg,
              static_cast<char*>(nullptr));
      else
        execl("/bin/sh", "sh", "-c", script.c_str(),
              static_cast<char*>(nullptr));
      _exit(127);
    }
    return pid;
  };
  pid_t zygote = spawn("--type=zygote", "30");
  pid_t own = spawn(nullptr, "30");
  // macOS's code-sign-clone-cleanup polls for the browser's exit.
  pid_t clone_cleanup = spawn("--type=code-sign-clone-cleanup", "30");
  EXPECT(zygote > 0 && own > 0 && clone_cleanup > 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // exec'd
  EXPECT(WaitForChildProcessesToExit(0) == 0);
  pid_t sleeper = spawn("--type=utility", "0.4");
  EXPECT(sleeper > 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // exec'd
  EXPECT(WaitForChildProcessesToExit(50) == 1);
  EXPECT(WaitForChildProcessesToExit(20000) == 0);  // a zombie has ended
  // It ended when the helper did, while the zygote, the clone cleanup and
  // the app's own child still run.
  EXPECT(kill(zygote, 0) == 0 && kill(own, 0) == 0 &&
         kill(clone_cleanup, 0) == 0);
  waitpid(sleeper, &status, 0);

  // The renderers a closed window leaves: the wait for one type pumps
  // between checks and ends when they have, whatever else still runs (the
  // zygote, the clone cleanup, the app's own child, a utility).
  pid_t utility = spawn("--type=utility", "30");
  pid_t renderer = spawn("--type=renderer", "0.4");
  EXPECT(utility > 0 && renderer > 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // exec'd
  int pumped = 0;
  auto pump = [&pumped]() { ++pumped; };
  EXPECT(WaitForCefHelpersOfType("renderer", 0, pump) == 1);
  EXPECT(WaitForCefHelpersOfType("renderer", 20000, pump) == 0);
  EXPECT(pumped > 0);
  EXPECT(kill(utility, 0) == 0);
  // None running: it returns at once, without pumping.
  pumped = 0;
  EXPECT(WaitForCefHelpersOfType("renderer", 20000, pump) == 0);
  EXPECT(pumped == 0);
  EXPECT(WaitForCefHelpersOfType("gpu-process", 0, nullptr) == 0);
  waitpid(renderer, &status, 0);
  kill(utility, SIGKILL);
  waitpid(utility, &status, 0);

  kill(zygote, SIGKILL);
  kill(own, SIGKILL);
  kill(clone_cleanup, SIGKILL);
  waitpid(zygote, &status, 0);
  waitpid(own, &status, 0);
  waitpid(clone_cleanup, &status, 0);

  std::string cmd = "/bin/rm -rf '" + root + "'";
  (void)!std::system(cmd.c_str());
#endif
  std::printf("cef_profile_lock_test: ok\n");
  return 0;
}
