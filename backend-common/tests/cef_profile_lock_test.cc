// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_cef_profile_lock.h: the profile's LevelDB LOCK files, the probe
// against a lock another process holds, the bounded wait, and the wait for
// child processes.

#include "laufey_cef_profile_lock.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
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
  EXPECT(!LevelDbLockHeld("/nonexistent/laufey/LOCK"));

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

  // Child processes: a running one is waited for; an ended one isn't.
  pid_t sleeper = fork();
  EXPECT(sleeper >= 0);
  if (sleeper == 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    _exit(0);
  }
  EXPECT(WaitForChildProcessesToExit(50) == 1);
  EXPECT(WaitForChildProcessesToExit(5000) == 0);  // a zombie has ended
  waitpid(sleeper, &status, 0);

  std::string cmd = "/bin/rm -rf '" + root + "'";
  (void)!std::system(cmd.c_str());
#endif
  std::printf("cef_profile_lock_test: ok\n");
  return 0;
}
