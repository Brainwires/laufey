// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// CEF on macOS: the app's own Safe Storage item (laufey_cef_keychain.h).

#include "laufey_cef_keychain.h"

#import <CommonCrypto/CommonCrypto.h>
#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <sqlite3.h>

#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "laufey_launch_config.h"

namespace laufey_common {

CodeSigningFacts CodeSigningFactsOfSelf() {
  CodeSigningFacts facts;
  SecCodeRef code = nullptr;
  if (SecCodeCopySelf(kSecCSDefaultFlags, &code) != errSecSuccess || !code)
    return facts;
  SecStaticCodeRef static_code = nullptr;
  if (SecCodeCopyStaticCode(code, kSecCSDefaultFlags, &static_code) !=
          errSecSuccess ||
      !static_code) {
    CFRelease(code);
    return facts;
  }
  CFDictionaryRef info = nullptr;
  if (SecCodeCopySigningInformation(static_code, kSecCSSigningInformation,
                                    &info) == errSecSuccess &&
      info) {
    NSDictionary* dict = (__bridge NSDictionary*)info;
    NSNumber* flags = dict[(__bridge NSString*)kSecCodeInfoFlags];
    id identifier = dict[(__bridge NSString*)kSecCodeInfoIdentifier];
    facts.is_signed = identifier != nil;
    if (flags)
      facts.adhoc = (flags.unsignedIntValue & kSecCodeSignatureAdhoc) != 0;
    NSString* team = dict[(__bridge NSString*)kSecCodeInfoTeamIdentifier];
    if ([team isKindOfClass:[NSString class]])
      facts.team_id = team.UTF8String ? team.UTF8String : "";
    CFRelease(info);
  }
  CFRelease(static_code);
  CFRelease(code);
  return facts;
}

namespace {

std::mutex g_mode_mutex;
bool g_mode_decided = false;
CefKeychainMode g_mode = CefKeychainMode::kMock;

std::mutex g_rename_mutex;
std::atomic<bool> g_rename_on{false};
NSString* g_rename_service = nil;
NSString* g_rename_account = nil;

}  // namespace

CefKeychainMode ProcessCefKeychainMode() {
  std::lock_guard<std::mutex> lock(g_mode_mutex);
  if (!g_mode_decided) {
    const char* force = std::getenv("LAUFEY_TEST_KEYCHAIN_APP_SCOPED");
    g_mode = DecideCefKeychainMode(CodeSigningFactsOfSelf(), LaunchAppId(),
                                   force && std::string(force) == "1");
    g_mode_decided = true;
  }
  return g_mode;
}

void SetCefKeychainFallback(const std::string& reason) {
  std::lock_guard<std::mutex> lock(g_mode_mutex);
  g_mode_decided = true;
  if (g_mode == CefKeychainMode::kMock)
    return;
  g_mode = CefKeychainMode::kMock;
  std::cerr << "laufey: keeping Chromium's mock keychain for this launch ("
            << reason << "): cookies are stored obfuscated, not under a key "
            << "of the app's own" << std::endl;
}

void EnableSafeStorageRename(const std::string& service,
                             const std::string& account) {
  std::lock_guard<std::mutex> lock(g_rename_mutex);
  g_rename_service = [NSString stringWithUTF8String:service.c_str()];
  g_rename_account = [NSString stringWithUTF8String:account.c_str()];
  g_rename_on.store(g_rename_service && g_rename_account);
}

const void* CopySafeStorageQueryRenamed(const void* raw) {
  if (!raw || !g_rename_on.load())
    return nullptr;
  @autoreleasepool {
    NSDictionary* dict = (__bridge NSDictionary*)raw;
    if (![dict isKindOfClass:[NSDictionary class]])
      return nullptr;
    id cls = dict[(__bridge NSString*)kSecClass];
    id service = dict[(__bridge NSString*)kSecAttrService];
    id account = dict[(__bridge NSString*)kSecAttrAccount];
    if (![cls isEqual:(__bridge id)kSecClassGenericPassword] ||
        ![service isKindOfClass:[NSString class]] ||
        ![service isEqualToString:@(kChromiumSafeStorageService)])
      return nullptr;
    // Chromium names its account with the service; a query without one
    // still targets the item.
    if (account && (![account isKindOfClass:[NSString class]] ||
                    ![account isEqualToString:@(kChromiumSafeStorageAccount)]))
      return nullptr;
    NSMutableDictionary* renamed = [dict mutableCopy];
    {
      std::lock_guard<std::mutex> lock(g_rename_mutex);
      renamed[(__bridge NSString*)kSecAttrService] = g_rename_service;
      if (account)
        renamed[(__bridge NSString*)kSecAttrAccount] = g_rename_account;
      if (dict[(__bridge NSString*)kSecAttrLabel])
        renamed[(__bridge NSString*)kSecAttrLabel] = g_rename_service;
    }
    return CFBridgingRetain(renamed);
  }
}

std::vector<uint8_t> OsCryptKeyForPassword(const std::string& password) {
  static const uint8_t kSalt[] = {'s', 'a', 'l', 't', 'y', 's', 'a', 'l', 't'};
  std::vector<uint8_t> key(kCCKeySizeAES128);
  CCKeyDerivationPBKDF(kCCPBKDF2, password.data(), password.size(), kSalt,
                       sizeof(kSalt), kCCPRFHmacAlgSHA1, 1003, key.data(),
                       key.size());
  return key;
}

namespace {

constexpr uint8_t kV10[] = {'v', '1', '0'};

bool Crypt(CCOperation op, const std::vector<uint8_t>& key, const uint8_t* in,
           size_t in_len, std::vector<uint8_t>* out) {
  static const uint8_t kIv[kCCBlockSizeAES128] = {
      ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
      ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
  // Decrypting, the padding is checked here, not by CommonCrypto: CCCrypt's
  // PKCS#7 removal looks at the last byte only, so a cookie sealed under
  // another key often "opens" with the mock key (a final byte of 0x01..0x10
  // is likely) and the migration would re-encrypt garbage. Every padding
  // byte must match, as in BoringSSL, which Chromium decrypts with.
  const bool decrypt = op == kCCDecrypt;
  if (decrypt && (in_len == 0 || in_len % kCCBlockSizeAES128 != 0))
    return false;
  out->assign(in_len + kCCBlockSizeAES128, 0);
  size_t moved = 0;
  CCCryptorStatus status = CCCrypt(
      op, kCCAlgorithmAES128, decrypt ? 0 : kCCOptionPKCS7Padding, key.data(),
      key.size(), kIv, in, in_len, out->data(), out->size(), &moved);
  if (status != kCCSuccess)
    return false;
  out->resize(moved);
  if (decrypt) {
    if (moved == 0)
      return false;
    const uint8_t pad = out->back();
    if (pad == 0 || pad > kCCBlockSizeAES128 || pad > moved)
      return false;
    for (size_t i = moved - pad; i < moved; ++i) {
      if ((*out)[i] != pad)
        return false;
    }
    out->resize(moved - pad);
  }
  return true;
}

}  // namespace

bool OsCryptDecryptV10(const std::vector<uint8_t>& key,
                       const std::vector<uint8_t>& encrypted,
                       std::vector<uint8_t>* plain) {
  if (encrypted.size() < sizeof(kV10) ||
      memcmp(encrypted.data(), kV10, sizeof(kV10)) != 0)
    return false;
  return Crypt(kCCDecrypt, key, encrypted.data() + sizeof(kV10),
               encrypted.size() - sizeof(kV10), plain);
}

std::vector<uint8_t> OsCryptEncryptV10(const std::vector<uint8_t>& key,
                                       const std::vector<uint8_t>& plain) {
  std::vector<uint8_t> body;
  if (!Crypt(kCCEncrypt, key, plain.data(), plain.size(), &body))
    return {};
  std::vector<uint8_t> out(kV10, kV10 + sizeof(kV10));
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

namespace {

// The app's item, looked up without any UI.
OSStatus FindItem(NSString* service, NSString* account) {
  NSDictionary* query = @{
    (id)kSecClass : (id)kSecClassGenericPassword,
    (id)kSecAttrService : service,
    (id)kSecAttrAccount : account,
    (id)kSecMatchLimit : (id)kSecMatchLimitOne,
  };
  return SecItemCopyMatching((__bridge CFDictionaryRef)query, nullptr);
}

std::string RandomPassword() {
  // As Chromium makes one: 128 random bits, base64.
  uint8_t bytes[16];
  if (SecRandomCopyBytes(kSecRandomDefault, sizeof(bytes), bytes) != 0)
    return "";
  NSData* data = [NSData dataWithBytes:bytes length:sizeof(bytes)];
  NSString* b64 = [data base64EncodedStringWithOptions:0];
  return b64.UTF8String ? b64.UTF8String : "";
}

// Re-encrypts one cookie database's "v10" rows inside an open transaction.
bool ReencryptDb(sqlite3* db, const std::vector<uint8_t>& from,
                 const std::vector<uint8_t>& to, CefKeychainMigration* out,
                 std::string* error) {
  sqlite3_stmt* select = nullptr;
  if (sqlite3_prepare_v2(db,
                         "SELECT rowid, encrypted_value FROM cookies WHERE "
                         "substr(encrypted_value, 1, 3) = x'763130'",
                         -1, &select, nullptr) != SQLITE_OK) {
    *error = sqlite3_errmsg(db);
    return false;
  }
  sqlite3_stmt* update = nullptr;
  if (sqlite3_prepare_v2(db,
                         "UPDATE cookies SET encrypted_value = ?1 WHERE "
                         "rowid = ?2",
                         -1, &update, nullptr) != SQLITE_OK) {
    *error = sqlite3_errmsg(db);
    sqlite3_finalize(select);
    return false;
  }
  bool ok = true;
  int rc;
  while ((rc = sqlite3_step(select)) == SQLITE_ROW) {
    sqlite3_int64 rowid = sqlite3_column_int64(select, 0);
    const uint8_t* blob =
        static_cast<const uint8_t*>(sqlite3_column_blob(select, 1));
    int len = sqlite3_column_bytes(select, 1);
    std::vector<uint8_t> encrypted(blob, blob + (len > 0 ? len : 0));
    std::vector<uint8_t> plain;
    if (!OsCryptDecryptV10(from, encrypted, &plain)) {
      out->skipped++;
      continue;
    }
    std::vector<uint8_t> again = OsCryptEncryptV10(to, plain);
    if (again.empty()) {
      *error = "AES encryption failed";
      ok = false;
      break;
    }
    sqlite3_bind_blob(update, 1, again.data(), static_cast<int>(again.size()),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(update, 2, rowid);
    if (sqlite3_step(update) != SQLITE_DONE) {
      *error = sqlite3_errmsg(db);
      ok = false;
      break;
    }
    sqlite3_reset(update);
    out->reencrypted++;
  }
  if (ok && rc != SQLITE_DONE && rc != SQLITE_ROW) {
    *error = sqlite3_errmsg(db);
    ok = false;
  }
  sqlite3_finalize(update);
  sqlite3_finalize(select);
  return ok;
}

bool HasCookiesTable(sqlite3* db) {
  sqlite3_stmt* stmt = nullptr;
  bool has = false;
  if (sqlite3_prepare_v2(db,
                         "SELECT 1 FROM sqlite_master WHERE type = 'table' "
                         "AND name = 'cookies'",
                         -1, &stmt, nullptr) == SQLITE_OK)
    has = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return has;
}

}  // namespace

CefKeychainMigration MigrateCefCookiesToAppKeychain(
    const std::string& root_cache_dir, const std::string& service,
    const std::string& account) {
  CefKeychainMigration out;
  @autoreleasepool {
    NSString* ns_service = [NSString stringWithUTF8String:service.c_str()];
    NSString* ns_account = [NSString stringWithUTF8String:account.c_str()];
    if (!ns_service || !ns_account) {
      out.error = "the app id is not UTF-8";
      return out;
    }
    OSStatus found = FindItem(ns_service, ns_account);
    if (found == errSecSuccess) {
      out.ok = true;  // Migrated (or created) by an earlier launch.
      return out;
    }
    if (found != errSecItemNotFound) {
      out.error = "the keychain did not answer for \"" + service +
                  "\" (OSStatus " + std::to_string(found) + ")";
      out.keychain_status = static_cast<int>(found);
      return out;
    }
    std::string password = RandomPassword();
    if (password.empty()) {
      out.error = "no random bytes for a new password";
      return out;
    }
    std::vector<uint8_t> from = OsCryptKeyForPassword(kMockKeychainPassword);
    std::vector<uint8_t> to = OsCryptKeyForPassword(password);

    // Each database inside its own transaction, all committed only once the
    // item is stored.
    std::vector<sqlite3*> open_dbs;
    auto rollback_all = [&] {
      for (sqlite3* db : open_dbs) {
        sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
        sqlite3_close_v2(db);
      }
      open_dbs.clear();
    };
    for (const char* rel : {"/Default/Cookies", "/Default/Network/Cookies",
                            "/Cookies", "/Network/Cookies"}) {
      if (root_cache_dir.empty())
        break;
      std::string path = root_cache_dir + rel;
      if (access(path.c_str(), F_OK) != 0)
        continue;
      sqlite3* db = nullptr;
      if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE,
                          nullptr) != SQLITE_OK) {
        out.error = path + ": " + (db ? sqlite3_errmsg(db) : "can't open");
        sqlite3_close_v2(db);
        rollback_all();
        return out;
      }
      sqlite3_busy_timeout(db, 2000);
      if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) !=
          SQLITE_OK) {
        out.error = path + ": " + sqlite3_errmsg(db);
        sqlite3_close_v2(db);
        rollback_all();
        return out;
      }
      open_dbs.push_back(db);
      std::string error;
      if (HasCookiesTable(db) && !ReencryptDb(db, from, to, &out, &error)) {
        out.error = path + ": " + error;
        rollback_all();
        return out;
      }
    }

    NSData* secret = [NSData dataWithBytes:password.data()
                                    length:password.size()];
    NSDictionary* attrs = @{
      (id)kSecClass : (id)kSecClassGenericPassword,
      (id)kSecAttrService : ns_service,
      (id)kSecAttrAccount : ns_account,
      (id)kSecAttrLabel : ns_service,
      (id)kSecValueData : secret,
    };
    OSStatus added = SecItemAdd((__bridge CFDictionaryRef)attrs, nullptr);
    if (added != errSecSuccess) {
      out.error = "storing \"" + service + "\" failed (OSStatus " +
                  std::to_string(added) + ")";
      out.keychain_status = static_cast<int>(added);
      rollback_all();
      return out;
    }
    for (sqlite3* db : open_dbs) {
      if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        // The item exists but this database keeps its mock-key cookies:
        // undo the item so the next launch tries again from the start.
        out.error = std::string("committing the cookies failed: ") +
                    sqlite3_errmsg(db);
        NSDictionary* query = @{
          (id)kSecClass : (id)kSecClassGenericPassword,
          (id)kSecAttrService : ns_service,
          (id)kSecAttrAccount : ns_account,
        };
        SecItemDelete((__bridge CFDictionaryRef)query);
        rollback_all();
        return out;
      }
    }
    for (sqlite3* db : open_dbs)
      sqlite3_close_v2(db);
    open_dbs.clear();
    out.ok = true;
    out.created_item = true;
  }
  return out;
}

}  // namespace laufey_common
