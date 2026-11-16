// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Windows unit tests for secret_store_win.cc, against the real Credential
// Manager of the logon session: bad arguments, the round trip, a value
// longer than one credential (split into parts, and the parts of a longer
// value gone when a shorter one replaces it, or the item is deleted), two app
// ids keeping separate items for one service and account, and a
// PasswordVault item moved on the first run of an app that ran before (not
// for a newly installed app, nor on a later run). A logon session without a
// credential store (a service or an SSH key logon) skips (77) unless
// LAUFEY_SECRET_TEST_REQUIRE=1. Run via `ctest --test-dir webview/build`.

#include "laufey_backend_common.h"
#include "laufey_secret_store.h"

#include <windows.h>

#include <roapi.h>
#include <wincred.h>
#include <windows.security.credentials.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace laufey_common;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
namespace wsc = ABI::Windows::Security::Credentials;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

static const uint32_t T = 2000;

// The credentials whose target names start with `prefix`.
static int CountTargets(const std::wstring& prefix) {
  DWORD count = 0;
  PCREDENTIALW* creds = nullptr;
  std::wstring filter = prefix + L"*";
  if (!CredEnumerateW(filter.c_str(), 0, &count, &creds))
    return 0;
  CredFree(creds);
  return static_cast<int>(count);
}

static ComPtr<wsc::IPasswordVault> Vault() {
  ComPtr<IInspectable> inspectable;
  if (FAILED(RoActivateInstance(
          HStringReference(
              RuntimeClass_Windows_Security_Credentials_PasswordVault)
              .Get(),
          &inspectable)))
    return nullptr;
  ComPtr<wsc::IPasswordVault> vault;
  inspectable.As(&vault);
  return vault;
}

static bool VaultAdd(const wchar_t* resource, const wchar_t* user,
                     const wchar_t* password) {
  ComPtr<wsc::IPasswordVault> vault = Vault();
  if (!vault)
    return false;
  ComPtr<IActivationFactory> af;
  if (FAILED(RoGetActivationFactory(
          HStringReference(
              RuntimeClass_Windows_Security_Credentials_PasswordCredential)
              .Get(),
          IID_PPV_ARGS(&af))))
    return false;
  ComPtr<wsc::ICredentialFactory> factory;
  if (FAILED(af.As(&factory)))
    return false;
  ComPtr<wsc::IPasswordCredential> cred;
  if (FAILED(factory->CreatePasswordCredential(
          HStringReference(resource).Get(), HStringReference(user).Get(),
          HStringReference(password).Get(), &cred)))
    return false;
  return SUCCEEDED(vault->Add(cred.Get()));
}

static bool VaultHas(const wchar_t* resource, const wchar_t* user) {
  ComPtr<wsc::IPasswordVault> vault = Vault();
  ComPtr<wsc::IPasswordCredential> cred;
  return vault &&
         SUCCEEDED(vault->Retrieve(HStringReference(resource).Get(),
                                   HStringReference(user).Get(), &cred)) &&
         cred;
}

static void VaultRemove(const wchar_t* resource, const wchar_t* user) {
  ComPtr<wsc::IPasswordVault> vault = Vault();
  ComPtr<wsc::IPasswordCredential> cred;
  if (vault &&
      SUCCEEDED(vault->Retrieve(HStringReference(resource).Get(),
                                HStringReference(user).Get(), &cred)) &&
      cred)
    vault->Remove(cred.Get());
}

static std::string TempDir(const char* name) {
  wchar_t base[MAX_PATH];
  GetTempPathW(MAX_PATH, base);
  std::string dir =
      WideToUtf8(base) + name + std::to_string(GetCurrentProcessId());
  return dir;
}

int main() {
  RoInitialize(RO_INIT_MULTITHREADED);
  std::string value, reason;
  SetSecretStoreAppForTesting("dev.laufey.secret-test.a", "", false);

  // Bad arguments fail before the store is touched.
  EXPECT(SecretLookup("", "a", T, &value, &reason) == SecretStatus::kFailed);
  EXPECT(SecretStore("svc", "", "", "v", T, &reason) == SecretStatus::kFailed);
  EXPECT(SecretDelete("svc", "\xff", T, &reason) == SecretStatus::kFailed);

  const std::string service = "laufey test/service#1";
  const std::string account = "acc%ount";
  SecretDelete(service, account, T, &reason);
  SecretStatus st = SecretStore(service, account, "Test", "s3cret", T, &reason);
  if (st == SecretStatus::kUnavailable) {
    std::fprintf(stderr, "no credential store here: %s\n", reason.c_str());
    const char* req = std::getenv("LAUFEY_SECRET_TEST_REQUIRE");
    return req && req[0] == '1' ? 1 : 77;
  }
  EXPECT(st == SecretStatus::kOk);
  EXPECT(SecretLookup(service, account, T, &value, &reason) ==
         SecretStatus::kOk);
  EXPECT(value == "s3cret");
  // The item is the app's: its target name carries the app id, encoded.
  const std::wstring prefix =
      L"laufey/dev.laufey.secret-test.a/laufey test%2Fservice%231/acc%25ount";
  EXPECT(CountTargets(prefix) == 1);

  // A value longer than one credential: split, read back whole.
  std::string big;
  for (int i = 0; i < 9000; ++i)
    big.push_back(static_cast<char>('a' + i % 26));
  EXPECT(SecretStore(service, account, "Test", big, T, &reason) ==
         SecretStatus::kOk);
  EXPECT(CountTargets(prefix) == 4);  // the item and three parts
  value.clear();
  EXPECT(SecretLookup(service, account, T, &value, &reason) ==
         SecretStatus::kOk);
  EXPECT(value == big);
  // A shorter value drops the parts; an empty one round-trips.
  EXPECT(SecretStore(service, account, "Test", "short", T, &reason) ==
         SecretStatus::kOk);
  EXPECT(CountTargets(prefix) == 1);
  EXPECT(SecretLookup(service, account, T, &value, &reason) ==
             SecretStatus::kOk &&
         value == "short");
  EXPECT(SecretStore(service, account, "", "", T, &reason) ==
         SecretStatus::kOk);
  EXPECT(SecretLookup(service, account, T, &value, &reason) ==
             SecretStatus::kOk &&
         value.empty());
  // Too large is refused.
  EXPECT(SecretStore(service, account, "", std::string(64 * 1024 + 1, 'x'), T,
                     &reason) == SecretStatus::kFailed);

  // Another app id doesn't see it, and keeps its own.
  SetSecretStoreAppForTesting("dev.laufey.secret-test.b", "", false);
  EXPECT(SecretLookup(service, account, T, &value, &reason) ==
         SecretStatus::kNotFound);
  EXPECT(SecretStore(service, account, "", "b's", T, &reason) ==
         SecretStatus::kOk);
  SetSecretStoreAppForTesting("dev.laufey.secret-test.a", "", false);
  EXPECT(SecretLookup(service, account, T, &value, &reason) ==
             SecretStatus::kOk &&
         value.empty());
  EXPECT(SecretStore(service, account, "", big, T, &reason) ==
         SecretStatus::kOk);
  // Deleting removes the item and its parts; deleting nothing is OK.
  EXPECT(SecretDelete(service, account, T, &reason) == SecretStatus::kOk);
  EXPECT(CountTargets(prefix) == 0);
  EXPECT(SecretDelete(service, account, T, &reason) == SecretStatus::kOk);
  EXPECT(SecretLookup(service, account, T, &value, &reason) ==
         SecretStatus::kNotFound);
  SetSecretStoreAppForTesting("dev.laufey.secret-test.b", "", false);
  EXPECT(SecretDelete(service, account, T, &reason) == SecretStatus::kOk);

  // PasswordVault items: moved only by the first run of an app that ran
  // before (its data directory existed, no marker yet).
  const wchar_t* vres = L"laufey-secret-test-vault";
  const wchar_t* vuser = L"token";
  VaultRemove(vres, vuser);
  if (!VaultAdd(vres, vuser, L"from the vault")) {
    std::fprintf(stderr, "no PasswordVault here; the move isn't checked\n");
  } else {
    std::string fresh_dir = TempDir("laufey-secret-fresh-");
    std::string old_dir = TempDir("laufey-secret-old-");
    CreateDirectoryW(Utf8ToWide(old_dir).c_str(), nullptr);
    // A newly installed app never takes it.
    SetSecretStoreAppForTesting("dev.laufey.secret-test.c", fresh_dir.c_str(),
                                false);
    EXPECT(SecretLookup("laufey-secret-test-vault", "token", T, &value,
                        &reason) == SecretStatus::kNotFound);
    EXPECT(VaultHas(vres, vuser));
    // The first run of an app that ran before takes it, and removes it from
    // the vault.
    SetSecretStoreAppForTesting("dev.laufey.secret-test.c", old_dir.c_str(),
                                true);
    EXPECT(SecretLookup("laufey-secret-test-vault", "token", T, &value,
                        &reason) == SecretStatus::kOk);
    EXPECT(value == "from the vault");
    EXPECT(!VaultHas(vres, vuser));
    EXPECT(SecretLookup("laufey-secret-test-vault", "token", T, &value,
                        &reason) == SecretStatus::kOk &&
           value == "from the vault");
    // A later run (the marker is there) moves nothing more.
    EXPECT(SecretDelete("laufey-secret-test-vault", "token", T, &reason) ==
           SecretStatus::kOk);
    EXPECT(VaultAdd(vres, vuser, L"again"));
    SetSecretStoreAppForTesting("dev.laufey.secret-test.c", old_dir.c_str(),
                                true);
    EXPECT(SecretLookup("laufey-secret-test-vault", "token", T, &value,
                        &reason) == SecretStatus::kNotFound);
    VaultRemove(vres, vuser);

    // During the first run, a key deleted before it was ever looked up (a
    // token cleared at sign-out) stays deleted: the delete removes the
    // vault's copy too, and the lookup after it finds nothing.
    std::string again_dir = TempDir("laufey-secret-again-");
    CreateDirectoryW(Utf8ToWide(again_dir).c_str(), nullptr);
    EXPECT(VaultAdd(vres, vuser, L"signed out"));
    SetSecretStoreAppForTesting("dev.laufey.secret-test.c", again_dir.c_str(),
                                true);
    EXPECT(SecretDelete("laufey-secret-test-vault", "token", T, &reason) ==
           SecretStatus::kOk);
    EXPECT(!VaultHas(vres, vuser));
    EXPECT(SecretLookup("laufey-secret-test-vault", "token", T, &value,
                        &reason) == SecretStatus::kNotFound);
    // And one the vault gets again later in that run (another program, an
    // older build still running) is not moved back either.
    EXPECT(VaultAdd(vres, vuser, L"resurrected"));
    EXPECT(SecretLookup("laufey-secret-test-vault", "token", T, &value,
                        &reason) == SecretStatus::kNotFound);
    VaultRemove(vres, vuser);
    DeleteFileW(Utf8ToWide(again_dir + "\\.laufey-secret-store-v2").c_str());
    RemoveDirectoryW(Utf8ToWide(again_dir).c_str());

    // A store during the first run replaces the vault's copy: it is removed,
    // and the stored value is what a lookup reads.
    std::string store_dir = TempDir("laufey-secret-store-");
    CreateDirectoryW(Utf8ToWide(store_dir).c_str(), nullptr);
    EXPECT(VaultAdd(vres, vuser, L"old token"));
    SetSecretStoreAppForTesting("dev.laufey.secret-test.c", store_dir.c_str(),
                                true);
    EXPECT(SecretStore("laufey-secret-test-vault", "token", "", "new token", T,
                       &reason) == SecretStatus::kOk);
    EXPECT(!VaultHas(vres, vuser));
    EXPECT(SecretLookup("laufey-secret-test-vault", "token", T, &value,
                        &reason) == SecretStatus::kOk &&
           value == "new token");
    EXPECT(SecretDelete("laufey-secret-test-vault", "token", T, &reason) ==
           SecretStatus::kOk);
    DeleteFileW(Utf8ToWide(store_dir + "\\.laufey-secret-store-v2").c_str());
    RemoveDirectoryW(Utf8ToWide(store_dir).c_str());

    DeleteFileW(Utf8ToWide(old_dir + "\\.laufey-secret-store-v2").c_str());
    RemoveDirectoryW(Utf8ToWide(old_dir).c_str());
    DeleteFileW(Utf8ToWide(fresh_dir + "\\.laufey-secret-store-v2").c_str());
    RemoveDirectoryW(Utf8ToWide(fresh_dir).c_str());
  }

  std::printf("secret_store_win_test: all passed\n");
  return 0;
}
