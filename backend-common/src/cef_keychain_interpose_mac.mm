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
//
// The dylib also carries the host's shutdown QoS interposer
// (LaufeyRaiseThreadsForShutdown): from the moment the host calls it, a
// thread of the CEF framework that asks for QOS_CLASS_BACKGROUND gets
// QOS_CLASS_USER_INITIATED, and every thread already in the process gets a
// QoS override to it.

#import <Security/Security.h>
#include <mach/mach.h>
#include <pthread.h>
#include <pthread/qos.h>

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

std::atomic<bool> g_shutting_down{false};

// Chromium's background thread type (QOS_CLASS_BACKGROUND,
// base/threading/platform_thread_apple.mm) becomes its default type
// (QOS_CLASS_USER_INITIATED) once the host has started shutting down: a
// thread at background QoS gets no CPU at all while threads of a higher QoS
// keep every core busy, and CefShutdown waits on such threads with no
// timeout (laufey_cef_keychain_interpose.h).
int LaufeyPthreadSetQosClassSelf(qos_class_t qos_class, int relative_priority) {
  if (qos_class == QOS_CLASS_BACKGROUND && g_shutting_down.load())
    qos_class = QOS_CLASS_USER_INITIATED;
  return pthread_set_qos_class_self_np(qos_class, relative_priority);
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
        {reinterpret_cast<const void*>(&LaufeyPthreadSetQosClassSelf),
         reinterpret_cast<const void*>(&pthread_set_qos_class_self_np)},
};

}  // namespace

extern "C" void LaufeySetSafeStorageRenamer(LaufeySafeStorageRenamer renamer) {
  g_renamer.store(renamer);
}

extern "C" int LaufeyRaiseThreadsForShutdown(void) {
  g_shutting_down.store(true);
  thread_act_array_t threads = nullptr;
  mach_msg_type_number_t count = 0;
  if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
    return 0;
  int raised = 0;
  for (mach_msg_type_number_t i = 0; i < count; ++i) {
    // An override only ever raises a thread's QoS, and lasts until the
    // process ends. A thread too new to have a pthread yet asks for its QoS
    // through the interposer above.
    pthread_t thread = pthread_from_mach_thread_np(threads[i]);
    if (thread && pthread_override_qos_class_start_np(
                      thread, QOS_CLASS_USER_INITIATED, 0) != nullptr)
      ++raised;
    mach_port_deallocate(mach_task_self(), threads[i]);
  }
  vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                count * sizeof(thread_act_t));
  return raised;
}
