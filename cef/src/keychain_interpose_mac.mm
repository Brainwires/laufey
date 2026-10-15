// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// dyld interposing of the SecItem* calls the CEF framework makes, so a
// team-signed app's OSCrypt password lives in an item of its own
// ("<app id> Safe Storage") instead of Chromium's shared "Chromium Safe
// Storage" (laufey_cef_keychain.h). The tuples live in the host executable,
// which dyld loads at launch, and apply to every image bound after it, the
// framework CEF loads included; the executable's own calls (the secure store,
// the cookie migration) are never interposed. With renaming off (the mock
// keychain) or any other item, the calls pass through unchanged.

#import <Security/Security.h>

#include "laufey_cef_keychain.h"

namespace {

// The renamed copy of `dict`, or nullptr to use `dict` as it is.
CFDictionaryRef Renamed(CFDictionaryRef dict) {
  return static_cast<CFDictionaryRef>(
      laufey_common::CopySafeStorageQueryRenamed(dict));
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
