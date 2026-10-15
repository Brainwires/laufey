// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// liblaufey_keychain_interpose.dylib (laufey_cef_keychain_interpose.h): this
// program links it, as the CEF host does, and dlopens a module that calls
// SecItemCopyMatching, as the host dlopens the CEF framework. The renamer
// must see that call, and the dictionary it returns must be the one that
// runs. The host's interposers once lived in its executable, where dyld
// ignores them: nothing was renamed and Chromium stored its shared item.
//
// Only lookups of an item that doesn't exist, asking for neither its data
// nor its attributes: nothing is stored or prompted for, and no keychain is
// needed (any status but the renamed query's
// errSecParam proves the original ran).

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "laufey_cef_keychain_interpose.h"

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

namespace {

std::string g_service;
std::atomic<int> g_seen{0};
std::atomic<bool> g_break{false};

// Counts the test's own lookups; with g_break, returns a copy whose class is
// not a class, which SecItemCopyMatching refuses with errSecParam.
const void* Renamer(const void* raw) {
  NSDictionary* dict = (__bridge NSDictionary*)raw;
  id service = dict[(__bridge NSString*)kSecAttrService];
  if (![service isKindOfClass:[NSString class]] ||
      ![service isEqualToString:@(g_service.c_str())])
    return nullptr;
  g_seen++;
  if (!g_break.load())
    return nullptr;
  NSMutableDictionary* broken = [dict mutableCopy];
  broken[(__bridge NSString*)kSecClass] = @"laufey-not-a-class";
  return CFBridgingRetain(broken);
}

}  // namespace

int main() {
  @autoreleasepool {
    g_service = "dev.laufey.interpose-test." + std::to_string(getpid());
    void* image = dlopen(LAUFEY_INTERPOSE_TEST_IMAGE, RTLD_NOW | RTLD_LOCAL);
    if (!image)
      std::fprintf(stderr, "dlopen: %s\n", dlerror());
    EXPECT(image != nullptr);
    using Lookup = int (*)(const char*);
    Lookup lookup =
        reinterpret_cast<Lookup>(dlsym(image, "LaufeyInterposeTestLookup"));
    EXPECT(lookup != nullptr);

    // No renamer: the call passes through.
    int plain = lookup(g_service.c_str());
    EXPECT(plain != errSecSuccess && plain != errSecParam);
    EXPECT(g_seen.load() == 0);

    // The renamer sees the dlopened image's call; returning nullptr keeps
    // the original query.
    LaufeySetSafeStorageRenamer(&Renamer);
    EXPECT(lookup(g_service.c_str()) == plain);
    EXPECT(g_seen.load() == 1);

    // Its renamed copy is what runs.
    g_break.store(true);
    EXPECT(lookup(g_service.c_str()) == errSecParam);
    EXPECT(g_seen.load() == 2);

    // Off again.
    LaufeySetSafeStorageRenamer(nullptr);
    EXPECT(lookup(g_service.c_str()) == plain);
    EXPECT(g_seen.load() == 2);

    dlclose(image);
    std::printf("laufey_keychain_interpose_mac_test: ok (lookup status %d)\n",
                plain);
  }
  return 0;
}
