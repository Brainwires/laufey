// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The secure store on macOS (API 47): a generic password per (service,
// account) in the Keychain, through Security.framework (SecItem*) in this
// process, so the item is the app's own. See laufey_secret_store.h and
// docs/secure-store.md.
//
// Which keychain:
//   - The data-protection keychain (kSecUseDataProtectionKeychain) when the
//     app is signed with an entitlement that names it
//     (com.apple.application-identifier or keychain-access-groups, which
//     macOS honours only with a provisioning profile). Its items belong to
//     the app's access group: no other app reads them, with or without a
//     prompt, and `security` does not see them. An item the app wrote to the
//     login keychain before it had the entitlement moves over on its first
//     lookup.
//   - Otherwise (Developer ID without a profile, ad-hoc, unsigned) the login
//     keychain, with an access list whose only trusted application is this
//     app (SecAccess + SecTrustedApplication of the calling executable: its
//     designated requirement when signed, so updates signed by the same
//     identity keep access; its exact code otherwise). Another program of the
//     same user, `security find-generic-password -w` included, gets macOS's
//     prompt (the keychain password, or the person's "Allow"), never the
//     secret silently. A rebuilt unsigned / ad-hoc app is another program to
//     macOS: its first lookup prompts.
//
// Items written here carry kSecAttrCreator 'Lfy1', and every query matches it:
// an item another program wrote for the same service and account (the
// `security` CLI an embedder used before) is never read here (no prompt for
// it: lookup is "not found"), and a store refuses while it is in the way.
// The embedder moves such items over with the tool that wrote them.
//
// Every call runs on one serial queue. A store or delete never prompts
// (user interaction is off while it runs: a locked keychain is refused);
// a lookup may (an unlock, or "allow" for a rebuilt ad-hoc app), and the
// caller stops waiting after its timeout. A call still queued when its caller
// gave up never runs, so nothing is written after the call gave up.

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <dispatch/dispatch.h>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "laufey_secret_store.h"

// SecAccess / SecTrustedApplication (the login keychain's access lists) and
// SecKeychainSetUserInteractionAllowed are deprecated with the file-based
// keychain itself, with no replacement for it; they still work and are what
// an app without the data-protection entitlement has.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace laufey_common {

namespace {

// kSecAttrCreator of the items written here.
constexpr FourCharCode kCreator = 'Lfy1';

uint32_t TimeoutOrDefault(uint32_t timeout_ms) {
  return timeout_ms ? timeout_ms : kSecretDefaultTimeoutMs;
}

NSString* Utf8(const std::string& s) {
  if (s.find('\0') != std::string::npos)
    return nil;
  return [[NSString alloc] initWithBytes:s.data()
                                  length:s.size()
                                encoding:NSUTF8StringEncoding];
}

bool Valid(const std::string& service, const std::string& account,
           std::string* reason) {
  if (service.empty() || account.empty()) {
    *reason = "the service and the account must not be empty";
    return false;
  }
  if (!Utf8(service) || !Utf8(account)) {
    *reason = "the service and the account must be UTF-8";
    return false;
  }
  return true;
}

std::string Message(OSStatus status) {
  NSString* text = CFBridgingRelease(SecCopyErrorMessageString(status, nullptr));
  std::string out = text.length ? std::string(text.UTF8String) : "error";
  return out + " (OSStatus " + std::to_string(static_cast<int>(status)) + ")";
}

// Whether this process is signed with an entitlement that gives it a
// data-protection keychain access group.
bool HasKeychainEntitlement() {
  SecTaskRef task = SecTaskCreateFromSelf(kCFAllocatorDefault);
  if (!task)
    return false;
  bool found = false;
  for (CFStringRef key : {CFSTR("keychain-access-groups"),
                          CFSTR("com.apple.application-identifier")}) {
    CFTypeRef value = SecTaskCopyValueForEntitlement(task, key, nullptr);
    if (value) {
      CFRelease(value);
      found = true;
      break;
    }
  }
  CFRelease(task);
  return found;
}

// Only touched on the queue.
bool g_data_protection_decided = false;
bool g_data_protection = false;

bool UseDataProtection() {
  if (!g_data_protection_decided) {
    g_data_protection = HasKeychainEntitlement();
    g_data_protection_decided = true;
  }
  return g_data_protection;
}

NSMutableDictionary* Query(NSString* service, NSString* account, bool dp) {
  NSMutableDictionary* q = [@{
    (id)kSecClass : (id)kSecClassGenericPassword,
    (id)kSecAttrService : service,
    (id)kSecAttrAccount : account,
    (id)kSecAttrCreator : @(kCreator),
  } mutableCopy];
  if (dp)
    q[(id)kSecUseDataProtectionKeychain] = @YES;
  return q;
}

// User interaction (keychain prompts) off for the scope: a store or delete
// is refused rather than wait for a person. Process-wide, so only while one
// call of this queue runs.
class NoPrompts {
 public:
  NoPrompts() {
    SecKeychainGetUserInteractionAllowed(&previous_);
    SecKeychainSetUserInteractionAllowed(false);
  }
  ~NoPrompts() { SecKeychainSetUserInteractionAllowed(previous_); }

 private:
  Boolean previous_ = true;
};

// What a refusal means, for the reason.
SecretStatus Refused(OSStatus status, const char* what, std::string* reason) {
  switch (status) {
    case errSecInteractionNotAllowed:
      *reason = std::string("the keychain can't ") + what +
                " without asking (it is locked, or this build of the app is "
                "not on the item's access list): " +
                Message(status);
      break;
    case errSecAuthFailed:
    case errSecInvalidOwnerEdit:
    case errSecUserCanceled:
      *reason = std::string("access to the keychain item was refused: ") +
                "this build of the app is not on its access list (an "
                "unsigned or ad-hoc build that changed asks the person on a "
                "lookup), or the person denied it: " +
                Message(status);
      break;
    case errSecNoDefaultKeychain:
    case errSecNoSuchKeychain:
      *reason = "there is no default keychain: " + Message(status);
      break;
    default:
      *reason = std::string("the keychain could not ") + what + ": " +
                Message(status);
      break;
  }
  return SecretStatus::kUnavailable;
}

// The login keychain's access list: this app, and nothing else, may read the
// item without a prompt.
SecAccessRef CreateOwnAccess(NSString* label, std::string* reason) {
  SecTrustedApplicationRef self_app = nullptr;
  OSStatus status = SecTrustedApplicationCreateFromPath(nullptr, &self_app);
  if (status != errSecSuccess || !self_app) {
    Refused(status, "name this app on the item's access list", reason);
    return nullptr;
  }
  NSArray* trusted = @[ (__bridge id)self_app ];
  SecAccessRef access = nullptr;
  status = SecAccessCreate((__bridge CFStringRef)label,
                           (__bridge CFArrayRef)trusted, &access);
  CFRelease(self_app);
  if (status != errSecSuccess || !access) {
    Refused(status, "create the item's access list", reason);
    return nullptr;
  }
  return access;
}

SecretStatus LookupIn(NSString* service, NSString* account, bool dp,
                      std::string* value, std::string* reason,
                      OSStatus* raw = nullptr) {
  NSMutableDictionary* q = Query(service, account, dp);
  q[(id)kSecReturnData] = @YES;
  q[(id)kSecMatchLimit] = (id)kSecMatchLimitOne;
  CFTypeRef out = nullptr;
  OSStatus status = SecItemCopyMatching((__bridge CFDictionaryRef)q, &out);
  if (raw)
    *raw = status;
  if (status == errSecItemNotFound)
    return SecretStatus::kNotFound;
  if (status != errSecSuccess)
    return Refused(status, "read the item", reason);
  NSData* data = CFBridgingRelease(out);
  NSString* text = [[NSString alloc] initWithData:data
                                         encoding:NSUTF8StringEncoding];
  if (!text) {
    *reason = "the stored value is not UTF-8 text";
    return SecretStatus::kFailed;
  }
  *value = std::string(static_cast<const char*>(data.bytes), data.length);
  return SecretStatus::kOk;
}

SecretStatus StoreIn(NSString* service, NSString* account, NSString* label,
                     NSData* data, bool dp, std::string* reason,
                     OSStatus* raw = nullptr) {
  NSMutableDictionary* q = Query(service, account, dp);
  OSStatus status = SecItemUpdate(
      (__bridge CFDictionaryRef)q,
      (__bridge CFDictionaryRef) @{
        (id)kSecValueData : data,
        (id)kSecAttrLabel : label,
      });
  if (raw)
    *raw = status;
  if (status == errSecSuccess)
    return SecretStatus::kOk;
  if (status != errSecItemNotFound)
    return Refused(status, "replace the item", reason);
  NSMutableDictionary* add = Query(service, account, dp);
  add[(id)kSecValueData] = data;
  add[(id)kSecAttrLabel] = label;
  if (dp) {
    // Readable once the Mac was unlocked after a restart (a background
    // refresh while the screen is locked still works), never synced or
    // restored to another Mac.
    add[(id)kSecAttrAccessible] =
        (id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly;
  } else {
    SecAccessRef access = CreateOwnAccess(label, reason);
    if (!access)
      return SecretStatus::kUnavailable;
    add[(id)kSecAttrAccess] = CFBridgingRelease(access);
  }
  status = SecItemAdd((__bridge CFDictionaryRef)add, nullptr);
  if (raw)
    *raw = status;
  if (status == errSecSuccess)
    return SecretStatus::kOk;
  if (status == errSecDuplicateItem) {
    *reason =
        "another program's keychain item for this service and account is in "
        "the way (one this app did not write, e.g. with `security "
        "add-generic-password`): delete it with the tool that wrote it first";
    return SecretStatus::kUnavailable;
  }
  return Refused(status, "add the item", reason);
}

SecretStatus DeleteIn(NSString* service, NSString* account, bool dp,
                      std::string* reason, OSStatus* raw = nullptr) {
  OSStatus status =
      SecItemDelete((__bridge CFDictionaryRef)Query(service, account, dp));
  if (raw)
    *raw = status;
  if (status == errSecSuccess || status == errSecItemNotFound)
    return SecretStatus::kOk;
  return Refused(status, "delete the item", reason);
}

// The data-protection keychain lacks the item: one this app wrote to the
// login keychain before it had the entitlement moves over (no prompt: this
// app is on its access list; a lookup that would have to ask is "not
// found").
SecretStatus MoveFromLoginKeychain(NSString* service, NSString* account,
                                   std::string* value, std::string* reason) {
  std::string old_value, ignored;
  SecretStatus found;
  {
    NoPrompts no_prompts;
    found = LookupIn(service, account, false, &old_value, &ignored);
  }
  if (found != SecretStatus::kOk)
    return SecretStatus::kNotFound;
  NoPrompts no_prompts;
  NSData* data = [NSData dataWithBytes:old_value.data()
                                length:old_value.size()];
  SecretStatus stored = StoreIn(service, account, service, data, true, reason);
  if (stored != SecretStatus::kOk)
    return stored;
  DeleteIn(service, account, false, &ignored);
  *value = old_value;
  return SecretStatus::kOk;
}

// One call's handoff between its caller and the queue.
struct Call {
  std::mutex mu;
  std::condition_variable cv;
  bool started = false;
  bool abandoned = false;
  bool done = false;
  SecretStatus status = SecretStatus::kFailed;
  std::string value;
  std::string reason;
};

dispatch_queue_t Queue() {
  static dispatch_queue_t queue =
      dispatch_queue_create("dev.laufey.secret-store", DISPATCH_QUEUE_SERIAL);
  return queue;
}

// Runs `work` on the queue and waits up to `timeout_ms` for it. A call that
// may prompt (a lookup) is given up on at the timeout; one that can't (a
// store or delete, with prompts off) is waited for once it started, so its
// answer is the truth about what was written.
SecretStatus RunOnQueue(
    uint32_t timeout_ms, bool may_prompt,
    std::function<SecretStatus(std::string* value, std::string* reason)> work,
    std::string* value, std::string* reason) {
  auto call = std::make_shared<Call>();
  dispatch_async(Queue(), ^{
    {
      std::lock_guard<std::mutex> lock(call->mu);
      if (call->abandoned)
        return;
      call->started = true;
    }
    std::string v, r;
    SecretStatus status;
    @autoreleasepool {
      status = work(&v, &r);
    }
    std::lock_guard<std::mutex> lock(call->mu);
    call->status = status;
    call->value = std::move(v);
    call->reason = std::move(r);
    call->done = true;
    call->cv.notify_all();
  });
  std::unique_lock<std::mutex> lock(call->mu);
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::milliseconds(TimeoutOrDefault(timeout_ms));
  if (!call->cv.wait_until(lock, deadline, [&] { return call->done; })) {
    if (call->started && !may_prompt) {
      call->cv.wait(lock, [&] { return call->done; });
    } else {
      call->abandoned = true;
      *reason =
          "the keychain did not answer within " +
          std::to_string(TimeoutOrDefault(timeout_ms)) +
          " ms (a keychain prompt is waiting for the person, or an earlier "
          "call is)";
      return SecretStatus::kUnavailable;
    }
  }
  if (value)
    *value = call->value;
  *reason = call->reason;
  return call->status;
}

char* Dup(const std::string& s) {
  char* out = static_cast<char*>(std::malloc(s.size() + 1));
  if (out)
    std::memcpy(out, s.c_str(), s.size() + 1);
  return out;
}

int Answer(SecretStatus status, const std::string& reason, char** reason_out) {
  if (reason_out)
    *reason_out = reason.empty() ? nullptr : Dup(reason);
  return static_cast<int>(status);
}

}  // namespace

SecretStatus SecretLookup(const std::string& service,
                          const std::string& account, uint32_t timeout_ms,
                          std::string* value, std::string* reason) {
  if (!Valid(service, account, reason))
    return SecretStatus::kFailed;
  NSString* s = Utf8(service);
  NSString* a = Utf8(account);
  return RunOnQueue(
      timeout_ms, /*may_prompt=*/true,
      [s, a](std::string* v, std::string* r) {
        bool dp = UseDataProtection();
        OSStatus raw = errSecSuccess;
        SecretStatus status = LookupIn(s, a, dp, v, r, &raw);
        if (dp && raw == errSecMissingEntitlement) {
          // The entitlement isn't usable after all (see SecretStore).
          g_data_protection = dp = false;
          r->clear();
          status = LookupIn(s, a, false, v, r);
        }
        if (status == SecretStatus::kNotFound && dp)
          return MoveFromLoginKeychain(s, a, v, r);
        return status;
      },
      value, reason);
}

SecretStatus SecretStore(const std::string& service, const std::string& account,
                         const std::string& label, const std::string& value,
                         uint32_t timeout_ms, std::string* reason) {
  if (!Valid(service, account, reason))
    return SecretStatus::kFailed;
  NSString* text = Utf8(value);
  if (!text) {
    *reason = "the value must be UTF-8 text";
    return SecretStatus::kFailed;
  }
  NSString* s = Utf8(service);
  NSString* a = Utf8(account);
  NSString* l = label.empty() ? s : (Utf8(label) ?: s);
  NSData* data = [NSData dataWithBytes:value.data() length:value.size()];
  return RunOnQueue(
      timeout_ms, /*may_prompt=*/false,
      [s, a, l, data](std::string*, std::string* r) {
        NoPrompts no_prompts;
        bool dp = UseDataProtection();
        OSStatus raw = errSecSuccess;
        SecretStatus status = StoreIn(s, a, l, data, dp, r, &raw);
        if (dp && raw == errSecMissingEntitlement) {
          // The entitlement isn't usable after all (a profile that doesn't
          // grant a keychain access group): the login keychain from now on.
          g_data_protection = false;
          r->clear();
          status = StoreIn(s, a, l, data, false, r);
        }
        if (status == SecretStatus::kOk && dp && g_data_protection) {
          // An older copy in the login keychain would otherwise come back on
          // a lookup after a delete.
          std::string ignored;
          DeleteIn(s, a, false, &ignored);
        }
        return status;
      },
      nullptr, reason);
}

SecretStatus SecretDelete(const std::string& service,
                          const std::string& account, uint32_t timeout_ms,
                          std::string* reason) {
  if (!Valid(service, account, reason))
    return SecretStatus::kFailed;
  NSString* s = Utf8(service);
  NSString* a = Utf8(account);
  return RunOnQueue(
      timeout_ms, /*may_prompt=*/false,
      [s, a](std::string*, std::string* r) {
        NoPrompts no_prompts;
        bool dp = UseDataProtection();
        OSStatus raw = errSecSuccess;
        SecretStatus status = DeleteIn(s, a, dp, r, &raw);
        if (dp && raw == errSecMissingEntitlement) {
          // The entitlement isn't usable after all (see SecretStore).
          g_data_protection = dp = false;
          r->clear();
          status = DeleteIn(s, a, false, r);
        }
        // A copy this app left in the login keychain before it had the
        // entitlement would come back on the next lookup.
        if (status == SecretStatus::kOk && dp)
          status = DeleteIn(s, a, false, r);
        return status;
      },
      nullptr, reason);
}

int SecretLookupForAbi(const char* service, const char* account,
                       uint32_t timeout_ms, char** value, char** reason) {
  if (value)
    *value = nullptr;
  std::string v, r;
  SecretStatus status = SecretLookup(
      service ? service : "", account ? account : "", timeout_ms, &v, &r);
  if (status == SecretStatus::kOk && value)
    *value = Dup(v);
  return Answer(status, r, reason);
}

int SecretStoreForAbi(const char* service, const char* account,
                      const char* label, const char* value, uint32_t timeout_ms,
                      char** reason) {
  std::string r;
  if (!value) {
    return Answer(SecretStatus::kFailed, "the value must not be NULL", reason);
  }
  SecretStatus status =
      SecretStore(service ? service : "", account ? account : "",
                  label ? label : "", value, timeout_ms, &r);
  return Answer(status, r, reason);
}

int SecretDeleteForAbi(const char* service, const char* account,
                       uint32_t timeout_ms, char** reason) {
  std::string r;
  SecretStatus status = SecretDelete(service ? service : "",
                                     account ? account : "", timeout_ms, &r);
  return Answer(status, r, reason);
}

const char* SecretKeychainKind() {
  __block bool dp = false;
  dispatch_sync(Queue(), ^{
    dp = UseDataProtection();
  });
  return dp ? "data-protection" : "login";
}

}  // namespace laufey_common
