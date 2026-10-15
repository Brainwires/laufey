// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_cef_keychain.h: when a CEF app gets a Safe Storage item of its own,
// the rename of Chromium's item, OSCrypt's "v10" encryption, and the one-time
// migration of a mock-keychain profile into the app's item.
//
// The migration part uses the login keychain: it stores an item named
// "<id> Safe Storage" for a test id, reads it back (this program created it,
// so no prompt), checks that another program (laufey_secret_store_mac_reader,
// prompts off) is refused it, and deletes it. Exits 77 (skipped) when there
// is no usable login keychain unless LAUFEY_SECRET_TEST_REQUIRE=1.

#include "laufey_cef_keychain.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern char** environ;

using namespace laufey_common;

namespace {

std::string g_service;
std::string g_account;
std::string g_dir;

void Cleanup() {
  if (!g_service.empty()) {
    NSDictionary* query = @{
      (id)kSecClass : (id)kSecClassGenericPassword,
      (id)kSecAttrService : @(g_service.c_str()),
      (id)kSecAttrAccount : @(g_account.c_str()),
    };
    SecItemDelete((__bridge CFDictionaryRef)query);
  }
  if (!g_dir.empty()) {
    std::string cmd = "/bin/rm -rf '" + g_dir + "'";
    (void)std::system(cmd.c_str());
  }
}

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      Cleanup();                                                             \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

std::vector<uint8_t> Bytes(const char* s) {
  return std::vector<uint8_t>(s, s + std::strlen(s));
}

// Runs argv, stdout captured.
std::string Run(const std::vector<std::string>& args) {
  int fds[2];
  if (pipe(fds) != 0)
    return "";
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&actions, fds[0]);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                   O_RDONLY, 0);
  std::vector<char*> argv;
  for (const std::string& a : args)
    argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid = 0;
  int spawned = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(),
                            environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  std::string out;
  if (spawned == 0) {
    char buf[1024];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof buf)) > 0)
      out.append(buf, static_cast<size_t>(n));
    int status = 0;
    waitpid(pid, &status, 0);
  }
  close(fds[0]);
  return out;
}

void Exec(sqlite3* db, const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    std::fprintf(stderr, "sqlite: %s\n", err ? err : "?");
    EXPECT(false);
  }
}

void InsertCookie(sqlite3* db, const char* name,
                  const std::vector<uint8_t>& value) {
  sqlite3_stmt* stmt = nullptr;
  EXPECT(sqlite3_prepare_v2(
             db, "INSERT INTO cookies (name, encrypted_value) VALUES (?1, ?2)",
             -1, &stmt, nullptr) == SQLITE_OK);
  sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
  sqlite3_bind_blob(stmt, 2, value.data(), static_cast<int>(value.size()),
                    SQLITE_TRANSIENT);
  EXPECT(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

std::vector<uint8_t> CookieValue(sqlite3* db, const char* name) {
  sqlite3_stmt* stmt = nullptr;
  EXPECT(sqlite3_prepare_v2(
             db, "SELECT encrypted_value FROM cookies WHERE name = ?1", -1,
             &stmt, nullptr) == SQLITE_OK);
  sqlite3_bind_text(stmt, 1, name, -1, SQLITE_TRANSIENT);
  std::vector<uint8_t> out;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const uint8_t* p = static_cast<const uint8_t*>(sqlite3_column_blob(stmt, 0));
    out.assign(p, p + sqlite3_column_bytes(stmt, 0));
  }
  sqlite3_finalize(stmt);
  return out;
}

}  // namespace

int main() {
  @autoreleasepool {
    // --- Which builds get an item of their own -----------------------------
    CodeSigningFacts team{true, false, "ABCDE12345"};
    CodeSigningFacts adhoc{true, true, ""};
    CodeSigningFacts unsigned_build{false, false, ""};
    EXPECT(DecideCefKeychainMode(team, "com.example.app", false) ==
           CefKeychainMode::kAppScoped);
    EXPECT(DecideCefKeychainMode(team, "", false) == CefKeychainMode::kMock);
    EXPECT(DecideCefKeychainMode(adhoc, "com.example.app", false) ==
           CefKeychainMode::kMock);
    EXPECT(DecideCefKeychainMode(unsigned_build, "com.example.app", false) ==
           CefKeychainMode::kMock);
    CodeSigningFacts adhoc_with_team{true, true, "ABCDE12345"};
    EXPECT(DecideCefKeychainMode(adhoc_with_team, "com.example.app", false) ==
           CefKeychainMode::kMock);
    EXPECT(DecideCefKeychainMode(unsigned_build, "com.example.app", true) ==
           CefKeychainMode::kAppScoped);
    EXPECT(DecideCefKeychainMode(unsigned_build, "", true) ==
           CefKeychainMode::kMock);
    EXPECT(AppSafeStorageService("com.example.app") ==
           "com.example.app Safe Storage");
    // This test program: whatever it is signed with, it reads its facts
    // without asking anyone.
    (void)CodeSigningFactsOfSelf();

    // --- Renaming Chromium's item ------------------------------------------
    NSDictionary* chromium_query = @{
      (id)kSecClass : (id)kSecClassGenericPassword,
      (id)kSecAttrService : @"Chromium Safe Storage",
      (id)kSecAttrAccount : @"Chromium",
      (id)kSecReturnData : @YES,
    };
    // Off until the browser process turns it on.
    EXPECT(CopySafeStorageQueryRenamed(
               (__bridge CFDictionaryRef)chromium_query) == nullptr);
    EnableSafeStorageRename("com.example.app Safe Storage", "com.example.app");
    CFDictionaryRef renamed = static_cast<CFDictionaryRef>(
        CopySafeStorageQueryRenamed((__bridge CFDictionaryRef)chromium_query));
    EXPECT(renamed != nullptr);
    NSDictionary* r = (__bridge NSDictionary*)renamed;
    EXPECT([r[(id)kSecAttrService]
        isEqualToString:@"com.example.app Safe Storage"]);
    EXPECT([r[(id)kSecAttrAccount] isEqualToString:@"com.example.app"]);
    EXPECT([r[(id)kSecReturnData] isEqual:@YES]);
    CFRelease(renamed);
    // Any other item passes through.
    for (NSDictionary* other in @[
           @{
             (id)kSecClass : (id)kSecClassGenericPassword,
             (id)kSecAttrService : @"com.example.secure-store",
             (id)kSecAttrAccount : @"Chromium",
           },
           @{
             (id)kSecClass : (id)kSecClassGenericPassword,
             (id)kSecAttrService : @"Chromium Safe Storage",
             (id)kSecAttrAccount : @"someone",
           },
           @{
             (id)kSecClass : (id)kSecClassInternetPassword,
             (id)kSecAttrService : @"Chromium Safe Storage",
           },
         ]) {
      EXPECT(CopySafeStorageQueryRenamed((__bridge CFDictionaryRef)other) ==
             nullptr);
    }

    // --- OSCrypt's v10 -------------------------------------------------------
    std::vector<uint8_t> mock = OsCryptKeyForPassword(kMockKeychainPassword);
    EXPECT(mock.size() == 16);
    std::vector<uint8_t> other_key = OsCryptKeyForPassword("other");
    EXPECT(mock != other_key);
    std::vector<uint8_t> sealed = OsCryptEncryptV10(mock, Bytes("hello"));
    EXPECT(sealed.size() == 3 + 16 && sealed[0] == 'v' && sealed[2] == '0');
    std::vector<uint8_t> opened;
    EXPECT(OsCryptDecryptV10(mock, sealed, &opened));
    EXPECT(opened == Bytes("hello"));
    EXPECT(!OsCryptDecryptV10(other_key, sealed, &opened) ||
           opened != Bytes("hello"));
    EXPECT(!OsCryptDecryptV10(mock, Bytes("v11xxxxxxxxxxxxxxxx"), &opened));
    // A value sealed under another key is refused, not "opened" to garbage:
    // CommonCrypto's own PKCS#7 removal accepts this one (its last byte
    // decrypts to 0x08 under the mock key, the 7 before it don't).
    EXPECT(!OsCryptDecryptV10(
        mock, OsCryptEncryptV10(other_key, Bytes("not mine")), &opened));
    // A damaged padding byte: in CBC, flipping the first block's last byte
    // flips the second block's last plaintext byte, the 14 bytes of padding
    // of an 18-byte value (0x0e -> 0x0f). And a body that isn't whole blocks.
    std::vector<uint8_t> damaged =
        OsCryptEncryptV10(mock, Bytes("0123456789abcdefXY"));
    EXPECT(damaged.size() == 3 + 32);
    EXPECT(OsCryptDecryptV10(mock, damaged, &opened));
    damaged[3 + 15] ^= 0x01;
    EXPECT(!OsCryptDecryptV10(mock, damaged, &opened));
    std::vector<uint8_t> short_body = sealed;
    short_body.pop_back();
    EXPECT(!OsCryptDecryptV10(mock, short_body, &opened));

    // --- The migration, in the login keychain ------------------------------
    g_account = "dev.laufey.cef-keychain-test." + std::to_string(getpid());
    g_service = AppSafeStorageService(g_account);
    char tmpl[] = "/tmp/laufey-cef-keychain-XXXXXX";
    EXPECT(mkdtemp(tmpl) != nullptr);
    g_dir = tmpl;
    EXPECT(mkdir((g_dir + "/Default").c_str(), 0700) == 0);
    std::string db_path = g_dir + "/Default/Cookies";
    sqlite3* db = nullptr;
    EXPECT(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);
    Exec(db, "CREATE TABLE cookies (name TEXT, encrypted_value BLOB)");
    InsertCookie(db, "session", OsCryptEncryptV10(mock, Bytes("s3cret")));
    InsertCookie(db, "foreign",
                 OsCryptEncryptV10(other_key, Bytes("not mine")));
    sqlite3_close(db);

    CefKeychainMigration m =
        MigrateCefCookiesToAppKeychain(g_dir, g_service, g_account);
    // No usable keychain: the lookup got no answer, or storing the item was
    // refused for want of one (no default or unlocked keychain, no UI to
    // unlock it with, no entitlement).
    const bool unusable =
        !m.ok && (m.error.find("did not answer") != std::string::npos ||
                  m.keychain_status == errSecInteractionNotAllowed ||
                  m.keychain_status == errSecNoDefaultKeychain ||
                  m.keychain_status == errSecNoSuchKeychain ||
                  m.keychain_status == errSecNotAvailable ||
                  m.keychain_status == errSecMissingEntitlement);
    if (unusable) {
      std::printf("laufey_cef_keychain_mac_test: no usable keychain: %s\n",
                  m.error.c_str());
      const char* require = std::getenv("LAUFEY_SECRET_TEST_REQUIRE");
      Cleanup();
      if (require && std::string(require) == "1")
        return 1;
      std::printf("laufey_cef_keychain_mac_test: skipped\n");
      return 77;
    }
    if (!m.ok)
      std::fprintf(stderr, "migration: %s\n", m.error.c_str());
    EXPECT(m.ok && m.created_item);
    if (m.reencrypted != 1 || m.skipped != 1)
      std::fprintf(stderr, "migration: %d re-encrypted, %d skipped\n",
                   m.reencrypted, m.skipped);
    EXPECT(m.reencrypted == 1 && m.skipped == 1);

    // The item, read back by this program (its creator: no prompt).
    NSDictionary* query = @{
      (id)kSecClass : (id)kSecClassGenericPassword,
      (id)kSecAttrService : @(g_service.c_str()),
      (id)kSecAttrAccount : @(g_account.c_str()),
      (id)kSecReturnData : @YES,
    };
    CFTypeRef data = nullptr;
    EXPECT(SecItemCopyMatching((__bridge CFDictionaryRef)query, &data) ==
           errSecSuccess);
    NSData* secret = (__bridge_transfer NSData*)data;
    std::string password(static_cast<const char*>(secret.bytes),
                         secret.length);
    EXPECT(password.size() == 24);  // base64 of 16 random bytes

    // The cookie it moved opens with the new key, not the mock one; the one
    // the mock key never opened is untouched.
    EXPECT(sqlite3_open(db_path.c_str(), &db) == SQLITE_OK);
    std::vector<uint8_t> moved = CookieValue(db, "session");
    EXPECT(OsCryptDecryptV10(OsCryptKeyForPassword(password), moved, &opened));
    EXPECT(opened == Bytes("s3cret"));
    EXPECT(!OsCryptDecryptV10(mock, moved, &opened) ||
           opened != Bytes("s3cret"));
    std::vector<uint8_t> untouched = CookieValue(db, "foreign");
    EXPECT(OsCryptDecryptV10(other_key, untouched, &opened));
    sqlite3_close(db);

    // A second launch finds the item and changes nothing.
    CefKeychainMigration again =
        MigrateCefCookiesToAppKeychain(g_dir, g_service, g_account);
    EXPECT(again.ok && !again.created_item && again.reencrypted == 0);

    // Another program of the user (prompts off) is refused the item.
#ifdef LAUFEY_SECRET_READER
    std::string out = Run({LAUFEY_SECRET_READER, g_service, g_account});
    EXPECT(out.find("status 0") == std::string::npos);
    EXPECT(out.find("value") == std::string::npos);
#endif

    Cleanup();
    std::printf("laufey_cef_keychain_mac_test: ok\n");
  }
  return 0;
}
