// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// dyld interposing of the SecItem* calls the CEF framework makes, so a
// team-signed app's OSCrypt password lives in an item of its own
// ("<app id> Safe Storage") instead of Chromium's shared "Chromium Safe
// Storage" (laufey_cef_keychain.h, laufey_cef_keychain_interpose.h).
//
// This file builds liblaufey_keychain_interpose.dylib, which the CEF host
// links, so dyld loads it at launch. dyld honors __interpose tuples only in a
// dylib, never in the main executable (the host's tuples were ignored there),
// and a launch-time dylib's tuples apply to every other image, the CEF
// framework the host dlopens later included. The calls made from this dylib
// itself are never interposed, so the replacements below reach the real
// functions. With no renamer set, or a query for any other item, the calls
// pass through unchanged.

#import <Security/Security.h>

#include <atomic>

#include "laufey_cef_keychain_interpose.h"

namespace {

std::atomic<LaufeySafeStorageRenamer> g_renamer{nullptr};

// The renamed copy of `dict`, or nullptr to use `dict` as it is.
CFDictionaryRef Renamed(CFDictionaryRef dict) {
  LaufeySafeStorageRenamer renamer = g_renamer.load();
  if (!renamer || !dict)
    return nullptr;
  return static_cast<CFDictionaryRef>(renamer(dict));
}

OSStatus LaufeySecItemCopyMatching(CFDictionaryRef query, CFTypeRef* result) {
  CFDictionaryRef renamed = Renamed(query);
  OSStatus status = SecItemCopyMatching(renamed ? renamed : query, result);
  if (renamed)
    CFRelease(renamed);
  return status;
}

OSStatus LaufeySecItemAdd(CFDictionaryRef attributes, CFTypeRef* result) {
  CFDictionaryRef renamed = Renamed(attributes);
  OSStatus status = SecItemAdd(renamed ? renamed : attributes, result);
  if (renamed)
    CFRelease(renamed);
  return status;
}

OSStatus LaufeySecItemUpdate(CFDictionaryRef query,
                             CFDictionaryRef attributes_to_update) {
  CFDictionaryRef renamed = Renamed(query);
  OSStatus status =
      SecItemUpdate(renamed ? renamed : query, attributes_to_update);
  if (renamed)
    CFRelease(renamed);
  return status;
}

OSStatus LaufeySecItemDelete(CFDictionaryRef query) {
  CFDictionaryRef renamed = Renamed(query);
  OSStatus status = SecItemDelete(renamed ? renamed : query);
  if (renamed)
    CFRelease(renamed);
  return status;
}

struct Interpose {
  const void* replacement;
  const void* replacee;
};

__attribute__((used)) const Interpose kLaufeyKeychainInterposers[]
    __attribute__((section("__DATA,__interpose"))) = {
        {reinterpret_cast<const void*>(&LaufeySecItemCopyMatching),
         reinterpret_cast<const void*>(&SecItemCopyMatching)},
        {reinterpret_cast<const void*>(&LaufeySecItemAdd),
         reinterpret_cast<const void*>(&SecItemAdd)},
        {reinterpret_cast<const void*>(&LaufeySecItemUpdate),
         reinterpret_cast<const void*>(&SecItemUpdate)},
        {reinterpret_cast<const void*>(&LaufeySecItemDelete),
         reinterpret_cast<const void*>(&SecItemDelete)},
};

}  // namespace

extern "C" void LaufeySetSafeStorageRenamer(LaufeySafeStorageRenamer renamer) {
  g_renamer.store(renamer);
}
