// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// A private Xvfb for the Linux tests that need an X display
// (dialog_quit_test, window_size_linux_test, file_chooser_dbus_test,
// clipboard_x11_test, window_blur_test).
//
// StartXvfb() returns only once the display takes a connection: Xvfb's
// -displayfd number alone is not enough, as a CI runner showed
// (gtk_init_check / xcb_connect failing at once on a display just reported
// ready, now and then on the aarch64 Linux runners). Each attempt connects
// to the display it was given, for up to 10 s; an attempt whose display
// never answers is stopped and a new Xvfb started, three times, and every
// failed attempt prints why (the connection error, Xvfb's own output), so
// a display that never comes up is a failure with its cause, not a skip.
// -noreset keeps the server from resetting when that check's connection
// closes, just before the test's own.
//
// StopXvfb() waits for the server to exit (SIGTERM, then SIGKILL after
// 5 s), so the next test never starts beside a server that is still going
// away.

#ifndef LAUFEY_BACKEND_COMMON_TESTS_XVFB_TEST_UTIL_H_
#define LAUFEY_BACKEND_COMMON_TESTS_XVFB_TEST_UTIL_H_

#include <fcntl.h>
#include <glib.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <xcb/xcb.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace laufey_test {

// Stops `pid` (an Xvfb this process started) and reaps it: SIGTERM, then
// SIGKILL if it is still there after 5 s. Not async-signal-safe.
inline void StopXvfb(pid_t pid) {
  if (pid <= 0)
    return;
  kill(pid, SIGTERM);
  for (int i = 0; i < 500; i++) {
    pid_t r = waitpid(pid, nullptr, WNOHANG);
    if (r == pid || (r < 0 && errno != EINTR))
      return;
    usleep(10 * 1000);
  }
  kill(pid, SIGKILL);
  while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {
  }
}

namespace xvfb_detail {

inline void PrintLog(const std::string& path) {
  gchar* text = nullptr;
  if (g_file_get_contents(path.c_str(), &text, nullptr, nullptr) && text &&
      *text)
    std::fprintf(stderr, "Xvfb's output:\n%s\n", text);
  g_free(text);
}

// One Xvfb: its pid once `display` (":N") takes a connection, else -1 with
// the reason on stderr (the server is stopped).
inline pid_t TryStart(const char* xvfb, const char* screen,
                      const std::string& log, std::string* display) {
  int fds[2];
  if (pipe(fds) != 0) {
    std::fprintf(stderr, "Xvfb: pipe: %s\n", std::strerror(errno));
    return -1;
  }
  // Built before fork(): the child only calls async-signal-safe functions.
  std::string fd = std::to_string(fds[1]);
  pid_t pid = fork();
  if (pid < 0) {
    std::fprintf(stderr, "Xvfb: fork: %s\n", std::strerror(errno));
    close(fds[0]);
    close(fds[1]);
    return -1;
  }
  if (pid == 0) {
    close(fds[0]);
    int null = open("/dev/null", O_RDWR);
    int out = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(null, STDIN_FILENO);
    dup2(out >= 0 ? out : null, STDOUT_FILENO);
    dup2(out >= 0 ? out : null, STDERR_FILENO);
    execl(xvfb, xvfb, "-displayfd", fd.c_str(), "-nolisten", "tcp", "-noreset",
          "-screen", "0", screen, static_cast<char*>(nullptr));
    _exit(127);
  }
  close(fds[1]);
  // The display number and a newline (two writes), then the fd closes.
  std::string number;
  gint64 end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  while (number.find('\n') == std::string::npos) {
    int left = static_cast<int>((end - g_get_monotonic_time()) / 1000);
    pollfd p = {fds[0], POLLIN, 0};
    if (left <= 0 || poll(&p, 1, left) != 1)
      break;
    char buf[32];
    ssize_t n = read(fds[0], buf, sizeof(buf));
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    number.append(buf, static_cast<size_t>(n));
  }
  close(fds[0]);
  size_t newline = number.find('\n');
  if (newline == std::string::npos || newline == 0) {
    std::fprintf(stderr, "Xvfb: no display number (read \"%s\")\n",
                 number.c_str());
    StopXvfb(pid);
    return -1;
  }
  *display = ":" + number.substr(0, newline);
  // The display must take a connection (and hand back its setup).
  int error = 0;
  do {
    xcb_connection_t* c = xcb_connect(display->c_str(), nullptr);
    error = xcb_connection_has_error(c);
    if (!error && !xcb_get_setup(c))
      error = -1;
    xcb_disconnect(c);
    if (!error)
      return pid;
    if (waitpid(pid, nullptr, WNOHANG) == pid) {
      std::fprintf(stderr, "Xvfb on %s exited before it took a connection\n",
                   display->c_str());
      return -1;
    }
    usleep(50 * 1000);
  } while (g_get_monotonic_time() < end);
  std::fprintf(stderr,
               "Xvfb on %s took no connection within 10 s (xcb error %d)\n",
               display->c_str(), error);
  StopXvfb(pid);
  return -1;
}

}  // namespace xvfb_detail

// Starts a private Xvfb (`screen` as WIDTHxHEIGHTxDEPTH) and points DISPLAY
// at it once it takes a connection; its pid, or -1 when there is no Xvfb
// (the test skips). An Xvfb that never comes up ends the process with 1.
inline pid_t StartXvfb(const char* screen) {
  gchar* xvfb = g_find_program_in_path("Xvfb");
  if (!xvfb)
    return -1;
  gchar* log = g_build_filename(g_get_tmp_dir(), "laufey-xvfb-XXXXXX", nullptr);
  int log_fd = g_mkstemp(log);
  if (log_fd >= 0)
    close(log_fd);
  for (int attempt = 1; attempt <= 3; attempt++) {
    std::string display;
    pid_t pid = xvfb_detail::TryStart(xvfb, screen, log, &display);
    if (pid > 0) {
      if (attempt > 1)
        std::fprintf(stderr, "Xvfb: up on %s at attempt %d\n", display.c_str(),
                     attempt);
      unlink(log);
      g_free(log);
      g_free(xvfb);
      setenv("DISPLAY", display.c_str(), 1);
      return pid;
    }
    xvfb_detail::PrintLog(log);
    std::fprintf(stderr, "Xvfb: attempt %d of 3 failed\n", attempt);
  }
  unlink(log);
  g_free(log);
  g_free(xvfb);
  std::fprintf(stderr, "Xvfb: no usable display after 3 attempts\n");
  std::fflush(stdout);
  std::_Exit(1);
}

}  // namespace laufey_test

#endif  // LAUFEY_BACKEND_COMMON_TESTS_XVFB_TEST_UTIL_H_
