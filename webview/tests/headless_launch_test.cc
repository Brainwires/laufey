// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// A headless launch (`<host> run <script>`: a forked worker, the updater's
// helper) runs its runtime to the end, however long it takes. The host
// (LAUFEY_HOST) is started with the stand-in runtime (headless_runtime_lib.cc)
// for 0 ms and for 12 s, longer than the bounded wait a windowed app's
// runtime gets once its loop has ended; each must write its marker and the
// host must exit 0 after it, not before.

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace {

bool RunHost(long runtime_ms) {
  char dir_template[] = "/tmp/laufey_headless_XXXXXX";
  const char* dir = mkdtemp(dir_template);
  if (!dir) {
    std::perror("mkdtemp");
    return false;
  }
  std::string marker = std::string(dir) + "/marker";
  auto start = std::chrono::steady_clock::now();
  pid_t pid = fork();
  if (pid == 0) {
    setenv("LAUFEY_RUNTIME_PATH", LAUFEY_HEADLESS_RUNTIME, 1);
    setenv("LAUFEY_TEST_HEADLESS_MARKER", marker.c_str(), 1);
    setenv("LAUFEY_TEST_HEADLESS_MS", std::to_string(runtime_ms).c_str(), 1);
    execl(LAUFEY_HOST, LAUFEY_HOST, "run", "probe.js",
          static_cast<char*>(nullptr));
    _exit(127);
  }
  if (pid < 0)
    return false;
  int status = 0;
  auto limit = std::chrono::milliseconds(runtime_ms) + std::chrono::seconds(30);
  while (waitpid(pid, &status, WNOHANG) != pid) {
    if (std::chrono::steady_clock::now() - start > limit) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
      std::fprintf(stderr, "headless_launch_test: the host hung (%ld ms)\n",
                   runtime_ms);
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::ifstream in(marker);
  std::stringstream text;
  text << in.rdbuf();
  unlink(marker.c_str());
  rmdir(dir);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::fprintf(stderr,
                 "headless_launch_test: the host ended with status %d "
                 "(%ld ms)\n",
                 WIFEXITED(status) ? WEXITSTATUS(status) : -1, runtime_ms);
    return false;
  }
  if (text.str() != "done") {
    std::fprintf(stderr,
                 "headless_launch_test: the host exited before its %ld ms "
                 "runtime finished\n",
                 runtime_ms);
    return false;
  }
  return true;
}

}  // namespace

int main() {
  for (long ms : {0L, 12000L}) {
    if (!RunHost(ms))
      return 1;
  }
  std::printf("headless_launch_test: ok\n");
  return 0;
}
