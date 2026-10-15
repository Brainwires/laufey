// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// CEF on macOS: liblaufey_keychain_interpose.dylib, the SecItem* interposers
// that rename Chromium's "Chromium Safe Storage" item to the app's own
// (laufey_cef_keychain.h). The CEF host links the dylib, which ships next to
// its executable (Contents/MacOS), because dyld applies __interpose tuples
// only from a dylib loaded at launch, never from the main executable.

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

#ifdef __cplusplus
}
#endif

#endif  // LAUFEY_CEF_KEYCHAIN_INTERPOSE_H_
