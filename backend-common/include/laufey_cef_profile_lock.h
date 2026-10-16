// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// CEF: a profile's LevelDB databases (localStorage, sessionStorage) are
// locked by the launch that has them open, through the LOCK file in each
// database's directory (base::File::Lock: an exclusive fcntl lock on POSIX,
// LockFileEx on Windows). The lock is the storage service's; where Chromium
// sandboxes that service (macOS) the browser process opens its files and
// takes the lock for it, so the host itself holds it until it exits. A
// launch that finds one still held (the previous launch's processes
// outliving its browser process, a quick relaunch) runs that database in
// memory only, and whatever the page stores is lost at exit. So:
//
//   - after CefShutdown the host waits (bounded) for its helper processes to
//     end (WaitForChildProcessesToExit);
//   - before CefInitialize it waits (bounded) for the profile's LevelDB LOCK
//     files to be free (WaitForProfileLevelDbLocks), and logs when it waited.

#ifndef LAUFEY_CEF_PROFILE_LOCK_H_
#define LAUFEY_CEF_PROFILE_LOCK_H_

#include <string>
#include <vector>

namespace laufey_common {

// The LOCK files of the profile's LevelDB databases under `root_cache_dir`
// (the default profile at the root or in Default/): Local Storage/leveldb
// and Session Storage. Pure string handling.
std::vector<std::string> ProfileLevelDbLockFiles(
    const std::string& root_cache_dir);

// Whether another process holds `lock_file`'s lock right now. A missing file
// is free. Probes by taking the lock and releasing it at once.
bool LevelDbLockHeld(const std::string& lock_file);

// Waits up to `timeout_ms` until no LOCK file of the profile is held.
// Returns how long it waited (0: free at once) and sets `still_held` to the
// files still held at the deadline.
int WaitForProfileLevelDbLocks(const std::string& root_cache_dir,
                               int timeout_ms,
                               std::vector<std::string>* still_held);

// Whether a child process with this command line (arguments separated by
// NULs, as /proc/<pid>/cmdline and KERN_PROCARGS2 give them, or by spaces,
// as on Windows) is a CEF helper the host waits for after shutdown: it has a
// --type= switch, and it is not a zygote, the crash handler or macOS's
// code-sign-clone-cleanup, which end only after the browser process does.
// The app's own child processes (a sidecar, a Deno.Command) have no --type=
// and are not waited for.
bool IsCefHelperToWaitFor(const std::string& command_line);

// Waits up to `timeout_ms` for this process's CEF helpers
// (IsCefHelperToWaitFor) to end; a zombie has ended. Returns the number
// still running.
int WaitForChildProcessesToExit(int timeout_ms);

// Both steps with their log lines, for the hosts.
void LaufeyWaitForProfileLocksBeforeInit(const std::string& root_cache_dir);
void LaufeyWaitForHelpersAfterShutdown(const std::string& root_cache_dir);

}  // namespace laufey_common

#endif  // LAUFEY_CEF_PROFILE_LOCK_H_
