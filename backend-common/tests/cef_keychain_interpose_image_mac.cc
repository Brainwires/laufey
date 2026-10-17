// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_keychain_interpose_mac_test's stand-in for the CEF framework: a
// module dlopened after launch whose SecItemCopyMatching and
// pthread_set_qos_class_self_np calls the dylib's interposers must reach. It
// looks an item up and returns the status; it never asks for the item's data
// or attributes.

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <pthread/qos.h>

extern "C" __attribute__((visibility("default"))) int LaufeyInterposeTestLookup(
    const char* service) {
  CFStringRef cf_service =
      CFStringCreateWithCString(nullptr, service, kCFStringEncodingUTF8);
  if (!cf_service)
    return errSecParam;
  // Neither data nor attributes: a lookup that can't prompt.
  const void* keys[] = {kSecClass, kSecAttrService, kSecMatchLimit};
  const void* values[] = {kSecClassGenericPassword, cf_service,
                          kSecMatchLimitOne};
  CFDictionaryRef query = CFDictionaryCreate(
      nullptr, keys, values, sizeof(keys) / sizeof(keys[0]),
      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  OSStatus status = SecItemCopyMatching(query, nullptr);
  CFRelease(query);
  CFRelease(cf_service);
  return static_cast<int>(status);
}

// What Chromium does for a thread of its background type.
extern "C" __attribute__((visibility("default"))) int
LaufeyInterposeTestSetBackgroundQos() {
  return pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
}
