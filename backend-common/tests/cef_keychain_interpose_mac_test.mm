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
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

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

// A thread's base scheduling priority (4 at background QoS).
int BasePriority(mach_port_t thread) {
  thread_extended_info_data_t info;
  mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
  if (thread_info(thread, THREAD_EXTENDED_INFO,
                  reinterpret_cast<thread_info_t>(&info),
                  &count) != KERN_SUCCESS)
    return -1;
  return info.pth_priority;
}

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

    // The shutdown QoS interposer (LaufeyRaiseThreadsForShutdown). Before
    // it, a thread of the image that asks for background QoS gets it.
    using SetBackgroundQos = int (*)();
    SetBackgroundQos set_background = reinterpret_cast<SetBackgroundQos>(
        dlsym(image, "LaufeyInterposeTestSetBackgroundQos"));
    EXPECT(set_background != nullptr);
    dispatch_semaphore_t demoted = dispatch_semaphore_create(0);
    dispatch_semaphore_t release = dispatch_semaphore_create(0);
    std::atomic<mach_port_t> background_port{0};
    std::atomic<int> background_qos{0};
    std::thread background([&] {
      EXPECT(set_background() == 0);
      background_qos.store(qos_class_self());
      background_port.store(pthread_mach_thread_np(pthread_self()));
      dispatch_semaphore_signal(demoted);
      dispatch_semaphore_wait(release, DISPATCH_TIME_FOREVER);
    });
    dispatch_semaphore_wait(demoted, DISPATCH_TIME_FOREVER);
    EXPECT(background_qos.load() == QOS_CLASS_BACKGROUND);
    const int before = BasePriority(background_port.load());
    EXPECT(before > 0);
    // It raises every thread already there, that one included (an
    // override: its requested QoS is unchanged, its priority is not)...
    EXPECT(LaufeyRaiseThreadsForShutdown() >= 2);
    const int after = BasePriority(background_port.load());
    EXPECT(after > before);
    // ...and from now on a request for background QoS gets the default.
    std::thread late([&] {
      EXPECT(set_background() == 0);
      EXPECT(qos_class_self() == QOS_CLASS_USER_INITIATED);
    });
    late.join();
    dispatch_semaphore_signal(release);
    background.join();

    dlclose(image);
    std::printf(
        "laufey_keychain_interpose_mac_test: ok (lookup status %d, background "
        "thread priority %d -> %d)\n",
        plain, before, after);
  }
  return 0;
}
