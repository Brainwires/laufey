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
//     files to be free (WaitForProfileLevelDbLocks), and logs when it waited;
//     then it cuts a write left half-done at the end of their logs
//     (RepairProfileLevelDbLogs, below).
//
// What the page wrote must also reach the database before the storage
// service ends. It commits a page's localStorage 5 s after a write, or at
// once when the page's renderer goes (StorageAreaImpl, OnNoBindings), and
// CefShutdown kills it without waiting for a commit in flight (Chromium
// waits only for the network service, which keeps cookies). The renderers
// of the closed windows are ended by tasks on the UI thread, which a stopped
// loop doesn't run, so CefShutdown ended them while the storage service was
// being killed. So on macOS, before CefShutdown, the host pumps CEF until
// they have ended (LaufeyWaitForRenderersBeforeShutdown).
//
// A write the kill cuts short leaves a torn record at the end of the
// database's log. LevelDB reuses the log on the next open and appends after
// it, and the open after that drops everything after the torn record in its
// 32 KiB block: every later launch's writes, until the block fills. So
// before CefInitialize the host cuts such an end (RepairProfileLevelDbLogs).

#ifndef LAUFEY_CEF_PROFILE_LOCK_H_
#define LAUFEY_CEF_PROFILE_LOCK_H_

#include <cstdint>
#include <functional>
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

// How much of a LevelDB log file (leveldb/db/log_format.h) to keep so that
// the next write appended to it can be read back: the length before a last
// record cut short by the end of the file (a header, a payload, or a FIRST
// fragment without its LAST), or before a record that doesn't check in the
// file's last block (LevelDB drops the rest of that block on open, and the
// writer that reuses the log appends after it). -1 when nothing needs to go
// (damage in an earlier block stays: the blocks after it still read).
int64_t LevelDbLogKeepLength(const std::vector<unsigned char>& log);

// Cuts a LevelDB log file to LevelDbLogKeepLength. Returns the bytes cut (0
// when none, or on any error).
int64_t RepairLevelDbLogTail(const std::string& log_file);

// RepairLevelDbLogTail for every log (<number>.log) of the profile's
// databases whose LOCK is free. Returns how many it cut, with a line on
// stderr for each.
int RepairProfileLevelDbLogs(const std::string& root_cache_dir);

// The --type= value of a child process's command line (separated as for
// IsCefHelperToWaitFor), "" when it has none.
std::string CefHelperType(const std::string& command_line);

// Waits up to `timeout_ms` for this process's running children of CEF type
// `type` (e.g. "renderer") to end, calling `pump` (when set) between checks.
// Returns the number still running.
int WaitForCefHelpersOfType(const std::string& type, int timeout_ms,
                            const std::function<void()>& pump);

// Both steps with their log lines, for the hosts.
void LaufeyWaitForProfileLocksBeforeInit(const std::string& root_cache_dir);
void LaufeyWaitForHelpersAfterShutdown(const std::string& root_cache_dir);
// Before CefShutdown: pumps CEF (`pump`) until the closed windows' renderer
// processes have ended, at most 2 s, and logs when they haven't.
void LaufeyWaitForRenderersBeforeShutdown(const std::function<void()>& pump);

}  // namespace laufey_common

#endif  // LAUFEY_CEF_PROFILE_LOCK_H_
