// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// CEF on macOS: where Chromium's cookie key (OSCrypt's "Safe Storage"
// password) lives.
//
// Chromium keeps that password in a login-keychain item whose name is fixed
// at Chromium's build time: service "Chromium Safe Storage", account
// "Chromium" (components/os_crypt/common/keychain_password_mac.mm). Every
// unbranded Chromium and CEF app shares that one item, so the second such app
// is asked whether it may read another app's item, and a denial deletes its
// cookies. That is why the CEF host runs with Chromium's mock keychain
// (--use-mock-keychain): a constant password, cookies only obfuscated.
//
// A team-signed app (a Developer ID or Apple Development identity, so its
// designated requirement, and with it the item's access list, survives a
// rebuild) gets an item of its own instead: the host interposes the SecItem*
// calls the CEF framework makes (dyld interposing from a dylib the host links,
// laufey_cef_keychain_interpose.h) and renames that item to service "<app id> Safe Storage", account
// "<app id>" (what Electron does in its source). The mock keychain is then
// off and platform_features reports "cookieEncryption": "os". Ad-hoc and
// unsigned builds keep the mock keychain: their access list names the
// build's code hash, so every rebuild would be asked for its own item.
//
// A profile written under the mock keychain is migrated once, before
// CefInitialize: its cookies are decrypted with the mock key and encrypted
// again with the new password, in one transaction, and the item is stored
// only if that commits (MigrateCefCookiesToAppKeychain). If anything fails,
// the launch keeps the mock keychain and says why on stderr; nothing is
// deleted.

#ifndef LAUFEY_CEF_KEYCHAIN_H_
#define LAUFEY_CEF_KEYCHAIN_H_

#include <cstdint>
#include <string>
#include <vector>

namespace laufey_common {

// What the running code's signature says (SecCodeCopySelf).
struct CodeSigningFacts {
  bool is_signed = false;
  bool adhoc = false;
  std::string team_id;
};

enum class CefKeychainMode {
  // --use-mock-keychain: the constant password.
  kMock,
  // The app's own "<app id> Safe Storage" item.
  kAppScoped,
};

// The keychain mode for a process with `facts` and `app_id`: app-scoped only
// for a team-signed (not ad-hoc) build with an app id. `test_force`
// (LAUFEY_TEST_KEYCHAIN_APP_SCOPED=1, the e2e battery) skips the signing
// check, never the app id.
inline CefKeychainMode DecideCefKeychainMode(const CodeSigningFacts& facts,
                                             const std::string& app_id,
                                             bool test_force) {
  if (app_id.empty())
    return CefKeychainMode::kMock;
  if (test_force)
    return CefKeychainMode::kAppScoped;
  if (!facts.is_signed || facts.adhoc || facts.team_id.empty())
    return CefKeychainMode::kMock;
  return CefKeychainMode::kAppScoped;
}

// Chromium's fixed item, and the app's.
inline constexpr char kChromiumSafeStorageService[] = "Chromium Safe Storage";
inline constexpr char kChromiumSafeStorageAccount[] = "Chromium";

inline std::string AppSafeStorageService(const std::string& app_id) {
  return app_id + " Safe Storage";
}

inline std::string AppSafeStorageAccount(const std::string& app_id) {
  return app_id;
}

// The password the mock keychain hands out (crypto/apple/fake_keychain_v2).
inline constexpr char kMockKeychainPassword[] = "mock_password";

#if defined(__APPLE__)

// This process's code signature.
CodeSigningFacts CodeSigningFactsOfSelf();

// The keychain mode this process runs with: decided once from its signature,
// the launch app id and LAUFEY_TEST_KEYCHAIN_APP_SCOPED, then lowered to
// kMock by SetCefKeychainFallback (a failed migration). Any process.
CefKeychainMode ProcessCefKeychainMode();
void SetCefKeychainFallback(const std::string& reason);

// Whether the SecItem* interposers rename Chromium's item, and to what.
// Set once by the browser process before CefInitialize.
void EnableSafeStorageRename(const std::string& service,
                             const std::string& account);

// The query/attributes dictionary with Chromium's item renamed, or nullptr
// when it does not name Chromium's item (or renaming is off). The caller
// releases a non-null result. CFDictionaryRef as `const void*` so the header
// stays plain C++.
const void* CopySafeStorageQueryRenamed(const void* dict);

// OSCrypt's key for `password` (PBKDF2-HMAC-SHA1, "saltysalt", 1003
// iterations, 16 bytes) and the "v10" AES-128-CBC encryption of a cookie
// value, as Chromium does it (IV of 16 spaces, PKCS#7; decrypting checks
// every padding byte, so a value sealed under another key is refused rather
// than "opened" to garbage). Exposed for tests.
std::vector<uint8_t> OsCryptKeyForPassword(const std::string& password);
bool OsCryptDecryptV10(const std::vector<uint8_t>& key,
                       const std::vector<uint8_t>& encrypted,
                       std::vector<uint8_t>* plain);
std::vector<uint8_t> OsCryptEncryptV10(const std::vector<uint8_t>& key,
                                       const std::vector<uint8_t>& plain);

// The outcome of the one-time migration.
struct CefKeychainMigration {
  bool ok = false;
  bool created_item = false;
  int reencrypted = 0;
  int skipped = 0;
  std::string error;
  // The keychain's OSStatus when a SecItem* call is what failed (0
  // otherwise), so a caller can tell an unusable keychain from a bug.
  int keychain_status = 0;
};

// Before CefInitialize, in the browser process of an app-scoped launch: if
// the app's item does not exist yet, re-encrypt the profile's "v10" cookies
// (Default/Cookies, Default/Network/Cookies under `root_cache_dir`) from the
// mock key to a new random password and store the item; a cookie the mock
// key does not open is left as it is. `ok` false: nothing changed, the launch
// must keep the mock keychain.
CefKeychainMigration MigrateCefCookiesToAppKeychain(
    const std::string& root_cache_dir, const std::string& service,
    const std::string& account);

#endif  // __APPLE__

}  // namespace laufey_common

#endif  // LAUFEY_CEF_KEYCHAIN_H_
