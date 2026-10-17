// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// CEF on macOS: liblaufey_keychain_interpose.dylib, the SecItem* interposers
// that rename Chromium's "Chromium Safe Storage" item to the app's own
// (laufey_cef_keychain.h), and the shutdown QoS interposer
// (LaufeyRaiseThreadsForShutdown). The CEF host links the dylib, which ships
// next to its executable (Contents/MacOS), because dyld applies __interpose
// tuples only from a dylib loaded at launch, never from the main executable.

#ifndef LAUFEY_CEF_KEYCHAIN_INTERPOSE_H_
#define LAUFEY_CEF_KEYCHAIN_INTERPOSE_H_

#ifdef __cplusplus
extern "C" {
#endif

// Returns a +1 renamed copy of a SecItem* query/attributes dictionary, or
// NULL to use it unchanged (laufey_common::CopySafeStorageQueryRenamed).
typedef const void* (*LaufeySafeStorageRenamer)(const void* dict);

// Every SecItemCopyMatching/Add/Update/Delete call in the process, except the
// dylib's own, goes through `renamer` from now on; NULL turns renaming off.
// Set by the browser process before CefInitialize.
__attribute__((visibility("default"))) void LaufeySetSafeStorageRenamer(
    LaufeySafeStorageRenamer renamer);

// Before CefShutdown (the browser process): from now on a request for
// QOS_CLASS_BACKGROUND from any image but the dylib gets
// QOS_CLASS_USER_INITIATED, and every thread already in the process gets a
// QoS override to QOS_CLASS_USER_INITIATED. Returns how many threads it
// raised.
//
// Why: Chromium runs its thread pool's BEST_EFFORT workers at
// QOS_CLASS_BACKGROUND, and macOS gives a background thread no CPU at all
// while threads of a higher QoS keep every core busy. CefShutdown waits,
// with no timeout, for the pool's BLOCK_SHUTDOWN tasks
// (TaskTracker::CompleteShutdown). Chromium raises its workers to their
// default type for shutdown, but each worker does that itself when it next
// runs, which a starved worker never does; nor does one that has just
// demoted itself on start (pthread_set_qos_class_self_np, still inside the
// call). With every core kept busy at the default QoS, the first quit stayed
// in CefShutdown past the exit e2e's 60 s limit.
__attribute__((visibility("default"))) int LaufeyRaiseThreadsForShutdown(void);

#ifdef __cplusplus
}
#endif

#endif  // LAUFEY_CEF_KEYCHAIN_INTERPOSE_H_
