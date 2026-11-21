// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The runtime's prestart call (LAUFEY_RUNTIME_PRESTART_SYMBOL in laufey.h).
//
// A runtime that confines the app's process with the OS's own sandbox
// (Landlock and seccomp on Linux, Seatbelt on macOS) has to do it before the
// process has a second thread: on Linux such a sandbox holds only the
// calling thread and the threads made after it. By the time a host starts
// the runtime (laufey_runtime_start) its web engine is running, with the
// toolkit's and the engine's threads, so that is too late.
//
// So a host calls this first thing in main(): after it knows which runtime
// library it loads and that the launch is the app's own (not a toast
// activation or a child process of the engine), and before the
// single-instance check, the toolkit and the web engine. The call loads the
// runtime library (the loader's later load finds it loaded) and calls the
// symbol if the library exports it. A runtime without it is unaffected.
//
// The runtime answers whether the app goes on starting. It may answer no
// with an exit code: an app that must not run unconfined and can't be
// confined here, or a launch another process takes over.
//
// POSIX only: on Windows the call does nothing (no in-process sandbox of
// this kind there).

#ifndef LAUFEY_RUNTIME_PRESTART_H_
#define LAUFEY_RUNTIME_PRESTART_H_

#include <laufey.h>

#include <string>

#ifndef _WIN32
#include <dlfcn.h>
#endif

namespace laufey_common {

// Calls the runtime's prestart, if `runtime_path` names a library that
// exports one. `engine` is LAUFEY_ENGINE_WEBVIEW or LAUFEY_ENGINE_CEF: the
// host's own. Returns true when the host goes on starting; false, with
// `*exit_code` set, when the process must exit now.
inline bool RuntimePrestart(const std::string& runtime_path,
                            int engine,
                            int* exit_code) {
#ifdef _WIN32
  (void)runtime_path;
  (void)engine;
  (void)exit_code;
  return true;
#else
  if (runtime_path.empty())
    return true;
  // The same flags as the loader's own load, which finds the library
  // loaded. A library that doesn't load is the loader's to report.
  void* handle = dlopen(runtime_path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle)
    return true;
  auto prestart = reinterpret_cast<laufey_runtime_prestart_fn>(
      dlsym(handle, LAUFEY_RUNTIME_PRESTART_SYMBOL));
  if (!prestart)
    return true;
  int code = 0;
  if (prestart(engine, &code) != 0)
    return true;
  if (exit_code)
    *exit_code = code;
  return false;
#endif
}

}  // namespace laufey_common

#endif  // LAUFEY_RUNTIME_PRESTART_H_
