// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The secure store (API 47) on Windows: the Credential Manager, through
// CredReadW / CredWriteW / CredDeleteW in the app's own process (generic
// credentials, CRED_PERSIST_LOCAL_MACHINE: this user on this machine, kept
// across logons, not roamed). Windows encrypts them with the user's DPAPI
// key; like the Secret Service on Linux it doesn't keep the programs of a
// user apart, so every item carries the app id in its target name, and one
// app never reads or overwrites another's (docs/secure-store.md).
//
// Target names: `laufey/<app id>/<service>/<account>`, each part with `%`,
// `/`, `#` and control characters percent-encoded (so the name is
// unambiguous; the app id is "" for an app without one). A credential holds
// at most CRED_MAX_CREDENTIAL_BLOB_SIZE (2560) bytes, so a longer value is
// split: the first part in the item itself, the rest in items named
// `<target>#<generation>/<n>`, and the item says how many there are and
// which generation (its `laufey/parts` attribute, "<count> <generation>"). A
// store writes the new generation's parts first and the item last, then
// deletes the old generation's parts, so a reader sees one whole value or
// the other.
//
// Items an older build of the app wrote with the Windows.Security.Credentials
// PasswordVault (resource = service, user name = account: how Deno Desktop's
// secure-store capability used it through PowerShell) are moved here by the
// app's lookups during the first run of the updated app, as on Linux: the
// first run whose app data directory has no `.laufey-secret-store-v2`
// marker, and only when that directory existed when the process started.
//
// Nothing here prompts or waits for a person: every call answers at once
// (the timeout is accepted, and bounds nothing that could block).

#include "laufey_secret_store.h"

#include "laufey_backend_common.h"
#include "laufey_launch_config.h"

#include <windows.h>

#include <roapi.h>
#include <wincred.h>
#include <windows.security.credentials.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace laufey_common {

namespace {

constexpr char kMigratedMarker[] = ".laufey-secret-store-v2";
constexpr wchar_t kPartsAttribute[] = L"laufey/parts";
// The bytes one credential holds (CRED_MAX_CREDENTIAL_BLOB_SIZE).
constexpr size_t kPartBytes = 5 * 512;
// The largest value stored: 64 KiB, in at most 26 credentials.
constexpr size_t kMaxValueBytes = 64 * 1024;

std::mutex& CallMutex() {
  static std::mutex m;
  return m;
}

bool IsDirectory(const std::string& path) {
  if (path.empty())
    return false;
  DWORD attrs = GetFileAttributesW(Utf8ToWide(path).c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Whether the app's data directory existed when the process started, before
// a backend created it (dynamic initialization, before main()).
const bool g_data_dir_at_start = IsDirectory(AppDataDir());

struct AppScope {
  std::string app_id;
  std::string data_dir;
  bool ran_before = false;
  bool decided = false;
  bool migrate = false;
};

// Under CallMutex.
AppScope& Scope() {
  static AppScope scope = [] {
    AppScope s;
    s.app_id = LaunchAppId();
    s.data_dir = AppDataDir();
    s.ran_before = g_data_dir_at_start;
    return s;
  }();
  return scope;
}

// Whether this run moves PasswordVault items (secret_store_linux.cc's rule).
// Under CallMutex.
bool MigrationWindow() {
  AppScope& s = Scope();
  if (s.decided)
    return s.migrate;
  s.decided = true;
  if (s.app_id.empty() || s.data_dir.empty())
    return false;
  std::wstring marker = Utf8ToWide(s.data_dir + "\\" + kMigratedMarker);
  if (GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES)
    return false;
  s.migrate = s.ran_before;
  if (EnsureDirectory(s.data_dir)) {
    HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
      CloseHandle(h);
  }
  return s.migrate;
}

bool ValidUtf8(const std::string& s) {
  if (s.empty())
    return true;
  return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                             static_cast<int>(s.size()), nullptr, 0) > 0;
}

bool Valid(const std::string& service, const std::string& account,
           std::string* reason) {
  if (service.empty() || account.empty()) {
    *reason = "the service and the account must not be empty";
    return false;
  }
  if (!ValidUtf8(service) || !ValidUtf8(account)) {
    *reason = "the service and the account must be UTF-8";
    return false;
  }
  return true;
}

std::string Encode(const std::string& part) {
  std::string out;
  for (unsigned char c : part) {
    if (c == '%' || c == '/' || c == '#' || c < 0x20 || c == 0x7f) {
      char buf[4];
      std::snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

std::wstring Target(const std::string& service, const std::string& account) {
  return Utf8ToWide("laufey/" + Encode(Scope().app_id) + "/" + Encode(service) +
                    "/" + Encode(account));
}

std::wstring PartTarget(const std::wstring& target,
                        const std::wstring& generation, size_t n) {
  return target + L"#" + generation + L"/" + std::to_wstring(n);
}

std::string ErrorText(DWORD error) {
  wchar_t* buf = nullptr;
  DWORD n = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
  std::string text;
  if (n && buf) {
    text = WideToUtf8(std::wstring(buf, n));
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                             text.back() == ' ' || text.back() == '.'))
      text.pop_back();
  }
  if (buf)
    LocalFree(buf);
  char code[32];
  std::snprintf(code, sizeof(code), " (error %lu)",
                static_cast<unsigned long>(error));
  return (text.empty() ? std::string("Windows error") : text) + code;
}

// Why the Credential Manager refused, as a reason.
SecretStatus Refused(const char* what, DWORD error, std::string* reason) {
  if (error == ERROR_NO_SUCH_LOGON_SESSION) {
    *reason = std::string(what) +
              ": this logon session has no credential store (a service or a "
              "network logon, such as SSH with a key, has none)";
    return SecretStatus::kUnavailable;
  }
  *reason = std::string(what) + ": " + ErrorText(error);
  return SecretStatus::kUnavailable;
}

// The parts layout of a read item: its count and generation ("1" and "" for
// a value in one credential).
struct Layout {
  size_t count = 1;
  std::wstring generation;
};

Layout LayoutOf(const CREDENTIALW* cred) {
  Layout l;
  for (DWORD i = 0; i < cred->AttributeCount; ++i) {
    const CREDENTIAL_ATTRIBUTEW& a = cred->Attributes[i];
    if (!a.Keyword || std::wcscmp(a.Keyword, kPartsAttribute) != 0)
      continue;
    std::string v(reinterpret_cast<const char*>(a.Value), a.ValueSize);
    size_t space = v.find(' ');
    if (space == std::string::npos)
      continue;
    unsigned long count = std::strtoul(v.substr(0, space).c_str(), nullptr, 10);
    if (count >= 1 && count <= kMaxValueBytes / kPartBytes + 1) {
      l.count = count;
      l.generation = Utf8ToWide(v.substr(space + 1));
    }
  }
  return l;
}

// Reads `target`: kOk with its bytes appended and its layout, kNotFound, or
// a refusal.
SecretStatus ReadCredential(const std::wstring& target, std::string* bytes,
                            Layout* layout, std::string* reason) {
  PCREDENTIALW cred = nullptr;
  if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
    DWORD error = GetLastError();
    if (error == ERROR_NOT_FOUND)
      return SecretStatus::kNotFound;
    return Refused("the Credential Manager didn't read it", error, reason);
  }
  if (cred->CredentialBlob && cred->CredentialBlobSize)
    bytes->append(reinterpret_cast<const char*>(cred->CredentialBlob),
                  cred->CredentialBlobSize);
  if (layout)
    *layout = LayoutOf(cred);
  CredFree(cred);
  return SecretStatus::kOk;
}

// The whole value of the item at `target`, its parts included.
SecretStatus ReadValue(const std::wstring& target, std::string* value,
                       Layout* layout_out, std::string* reason) {
  std::string bytes;
  Layout layout;
  SecretStatus s = ReadCredential(target, &bytes, &layout, reason);
  if (s != SecretStatus::kOk)
    return s;
  for (size_t n = 1; n < layout.count; ++n) {
    s = ReadCredential(PartTarget(target, layout.generation, n), &bytes,
                       nullptr, reason);
    if (s == SecretStatus::kNotFound) {
      *reason = "the stored value is incomplete (a part of it is missing)";
      return SecretStatus::kUnavailable;
    }
    if (s != SecretStatus::kOk)
      return s;
  }
  *value = std::move(bytes);
  if (layout_out)
    *layout_out = layout;
  return SecretStatus::kOk;
}

bool WriteCredential(const std::wstring& target, const std::wstring& user,
                     const std::wstring& comment, const char* bytes, size_t len,
                     const std::string* parts_attribute, DWORD* error) {
  CREDENTIALW cred = {};
  cred.Type = CRED_TYPE_GENERIC;
  cred.TargetName = const_cast<wchar_t*>(target.c_str());
  cred.UserName = const_cast<wchar_t*>(user.c_str());
  cred.Comment =
      comment.empty() ? nullptr : const_cast<wchar_t*>(comment.c_str());
  cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
  cred.CredentialBlobSize = static_cast<DWORD>(len);
  cred.CredentialBlob =
      len ? reinterpret_cast<LPBYTE>(const_cast<char*>(bytes)) : nullptr;
  CREDENTIAL_ATTRIBUTEW attr = {};
  if (parts_attribute) {
    attr.Keyword = const_cast<wchar_t*>(kPartsAttribute);
    attr.ValueSize = static_cast<DWORD>(parts_attribute->size());
    attr.Value =
        reinterpret_cast<LPBYTE>(const_cast<char*>(parts_attribute->data()));
    cred.AttributeCount = 1;
    cred.Attributes = &attr;
  }
  if (CredWriteW(&cred, 0))
    return true;
  *error = GetLastError();
  return false;
}

void DeleteParts(const std::wstring& target, const Layout& layout) {
  for (size_t n = 1; n < layout.count; ++n)
    CredDeleteW(PartTarget(target, layout.generation, n).c_str(),
                CRED_TYPE_GENERIC, 0);
}

std::wstring NewGeneration() {
  std::random_device rd;
  unsigned long long g = (static_cast<unsigned long long>(rd()) << 32) ^ rd();
  wchar_t buf[20];
  std::swprintf(buf, 20, L"%016llx", g);
  return buf;
}

// Under CallMutex.
SecretStatus StoreLocked(const std::string& service, const std::string& account,
                         const std::string& label, const std::string& value,
                         std::string* reason) {
  const std::wstring target = Target(service, account);
  // The layout being replaced, for its parts.
  std::string old_bytes;
  Layout old;
  bool had_old =
      ReadCredential(target, &old_bytes, &old, reason) == SecretStatus::kOk;
  reason->clear();
  const std::wstring user = Utf8ToWide(account);
  std::wstring comment = Utf8ToWide(label);
  if (comment.size() > CRED_MAX_STRING_LENGTH)
    comment.resize(CRED_MAX_STRING_LENGTH);
  size_t count =
      value.empty() ? 1 : (value.size() + kPartBytes - 1) / kPartBytes;
  Layout fresh;
  fresh.count = count;
  if (count > 1)
    fresh.generation = NewGeneration();
  DWORD error = 0;
  for (size_t n = 1; n < count; ++n) {
    size_t at = n * kPartBytes;
    size_t len = (std::min)(kPartBytes, value.size() - at);
    if (!WriteCredential(PartTarget(target, fresh.generation, n), user, L"",
                         value.data() + at, len, nullptr, &error)) {
      DeleteParts(target, Layout{n, fresh.generation});
      return Refused("the Credential Manager didn't store it", error, reason);
    }
  }
  std::string parts;
  if (count > 1)
    parts = std::to_string(count) + " " + WideToUtf8(fresh.generation);
  if (!WriteCredential(target, user, comment, value.data(),
                       (std::min)(kPartBytes, value.size()),
                       count > 1 ? &parts : nullptr, &error)) {
    DeleteParts(target, fresh);
    return Refused("the Credential Manager didn't store it", error, reason);
  }
  if (had_old && old.generation != fresh.generation)
    DeleteParts(target, old);
  return SecretStatus::kOk;
}

// --- Moving PasswordVault items ----------------------------------------------

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
namespace wsc = ABI::Windows::Security::Credentials;

// Initializes the Windows Runtime on this thread for the scope (or uses the
// apartment the thread already has).
class RoScope {
 public:
  RoScope() {
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    owned_ = SUCCEEDED(hr);
  }
  ~RoScope() {
    if (owned_)
      RoUninitialize();
  }

 private:
  bool owned_ = false;
};

ComPtr<wsc::IPasswordVault> OpenVault() {
  ComPtr<IInspectable> inspectable;
  if (FAILED(RoActivateInstance(
          HStringReference(
              RuntimeClass_Windows_Security_Credentials_PasswordVault)
              .Get(),
          &inspectable)))
    return nullptr;
  ComPtr<wsc::IPasswordVault> vault;
  if (FAILED(inspectable.As(&vault)))
    return nullptr;
  return vault;
}

// The PasswordVault password for (resource, user), removed from the vault
// once `keep` has stored it. False when there is none (or no vault).
template <typename Keep>
bool TakeFromPasswordVault(const std::string& service,
                           const std::string& account, Keep keep) {
  RoScope ro;
  ComPtr<wsc::IPasswordVault> vault = OpenVault();
  if (!vault)
    return false;
  std::wstring resource = Utf8ToWide(service);
  std::wstring user = Utf8ToWide(account);
  ComPtr<wsc::IPasswordCredential> cred;
  if (FAILED(vault->Retrieve(
          HStringReference(resource.c_str(), static_cast<UINT>(resource.size()))
              .Get(),
          HStringReference(user.c_str(), static_cast<UINT>(user.size())).Get(),
          &cred)) ||
      !cred)
    return false;
  if (FAILED(cred->RetrievePassword()))
    return false;
  HString password;
  if (FAILED(cred->get_Password(password.GetAddressOf())))
    return false;
  UINT32 len = 0;
  const wchar_t* raw = WindowsGetStringRawBuffer(password.Get(), &len);
  std::string value = WideToUtf8(std::wstring(raw ? raw : L"", len));
  if (!keep(value))
    return false;
  vault->Remove(cred.Get());
  return true;
}

// Removes the PasswordVault credential for (resource, user), if there is one:
// a delete or a store leaves no older copy behind for a lookup to move back.
void RemoveFromPasswordVault(const std::string& service,
                             const std::string& account) {
  RoScope ro;
  ComPtr<wsc::IPasswordVault> vault = OpenVault();
  if (!vault)
    return;
  std::wstring resource = Utf8ToWide(service);
  std::wstring user = Utf8ToWide(account);
  ComPtr<wsc::IPasswordCredential> cred;
  if (SUCCEEDED(vault->Retrieve(
          HStringReference(resource.c_str(), static_cast<UINT>(resource.size()))
              .Get(),
          HStringReference(user.c_str(), static_cast<UINT>(user.size())).Get(),
          &cred)) &&
      cred)
    vault->Remove(cred.Get());
}

// The keys deleted or stored during this run (app id, service, account):
// never moved from the vault afterwards, even when removing the vault's copy
// failed. Under CallMutex.
std::set<std::string>& Settled() {
  static std::set<std::string> keys;
  return keys;
}

std::string SettledKey(const std::string& service, const std::string& account) {
  return WideToUtf8(Target(service, account));
}

}  // namespace

SecretStatus SecretLookup(const std::string& service,
                          const std::string& account, uint32_t timeout_ms,
                          std::string* value, std::string* reason) {
  (void)timeout_ms;
  std::string r;
  if (!Valid(service, account, &r)) {
    if (reason)
      *reason = r;
    return SecretStatus::kFailed;
  }
  std::lock_guard<std::mutex> lock(CallMutex());
  bool migrate = MigrationWindow();
  std::string v;
  SecretStatus s = ReadValue(Target(service, account), &v, nullptr, &r);
  // Another process storing a longer value deletes the old parts once its
  // item names the new ones: read it again.
  if (s == SecretStatus::kUnavailable) {
    r.clear();
    s = ReadValue(Target(service, account), &v, nullptr, &r);
  }
  if (s == SecretStatus::kNotFound && migrate &&
      !Settled().count(SettledKey(service, account))) {
    std::string moved;
    std::string store_reason;
    if (TakeFromPasswordVault(service, account, [&](const std::string& old) {
          if (old.size() > kMaxValueBytes)
            return false;
          if (StoreLocked(service, account, "", old, &store_reason) !=
              SecretStatus::kOk)
            return false;
          moved = old;
          return true;
        })) {
      v = std::move(moved);
      s = SecretStatus::kOk;
    }
  }
  if (s == SecretStatus::kOk && value)
    *value = std::move(v);
  if (reason)
    *reason = s == SecretStatus::kOk || s == SecretStatus::kNotFound
                  ? std::string()
                  : r;
  return s;
}

SecretStatus SecretStore(const std::string& service, const std::string& account,
                         const std::string& label, const std::string& value,
                         uint32_t timeout_ms, std::string* reason) {
  (void)timeout_ms;
  std::string r;
  if (!Valid(service, account, &r)) {
    if (reason)
      *reason = r;
    return SecretStatus::kFailed;
  }
  if (value.size() > kMaxValueBytes) {
    if (reason)
      *reason = "the value is larger than the Windows store's 64 KiB";
    return SecretStatus::kFailed;
  }
  std::lock_guard<std::mutex> lock(CallMutex());
  MigrationWindow();
  SecretStatus s = StoreLocked(service, account, label, value, &r);
  if (s == SecretStatus::kOk) {
    // The value replaces any older copy an earlier build left in the vault.
    Settled().insert(SettledKey(service, account));
    RemoveFromPasswordVault(service, account);
  }
  if (reason)
    *reason = s == SecretStatus::kOk ? std::string() : r;
  return s;
}

SecretStatus SecretDelete(const std::string& service,
                          const std::string& account, uint32_t timeout_ms,
                          std::string* reason) {
  (void)timeout_ms;
  std::string r;
  if (!Valid(service, account, &r)) {
    if (reason)
      *reason = r;
    return SecretStatus::kFailed;
  }
  std::lock_guard<std::mutex> lock(CallMutex());
  MigrationWindow();
  // A copy an earlier build left in the PasswordVault goes too, and the key
  // is never moved back from there during this run: a deleted secret stays
  // deleted (even one deleted before it was ever looked up).
  Settled().insert(SettledKey(service, account));
  RemoveFromPasswordVault(service, account);
  const std::wstring target = Target(service, account);
  std::string bytes;
  Layout layout;
  SecretStatus s = ReadCredential(target, &bytes, &layout, &r);
  SecureZeroMemory(bytes.data(), bytes.size());
  if (s == SecretStatus::kNotFound) {
    if (reason)
      reason->clear();
    return SecretStatus::kOk;
  }
  if (s != SecretStatus::kOk) {
    if (reason)
      *reason = r;
    return s;
  }
  if (!CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) {
    DWORD error = GetLastError();
    if (error != ERROR_NOT_FOUND) {
      s = Refused("the Credential Manager didn't delete it", error, &r);
      if (reason)
        *reason = r;
      return s;
    }
  }
  DeleteParts(target, layout);
  if (reason)
    reason->clear();
  return SecretStatus::kOk;
}

void SetSecretStoreAppForTesting(const char* app_id, const char* data_dir,
                                 bool ran_before) {
  std::lock_guard<std::mutex> lock(CallMutex());
  AppScope& s = Scope();
  s.app_id = app_id ? app_id : "";
  s.data_dir = data_dir ? data_dir : "";
  s.ran_before = ran_before;
  s.decided = false;
  s.migrate = false;
  Settled().clear();
}

namespace {

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

int SecretLookupForAbi(const char* service, const char* account,
                       uint32_t timeout_ms, char** value, char** reason) {
  if (value)
    *value = nullptr;
  std::string v, r;
  SecretStatus status = SecretLookup(
      service ? service : "", account ? account : "", timeout_ms, &v, &r);
  if (status == SecretStatus::kOk && value)
    *value = Dup(v);
  SecureZeroMemory(v.data(), v.size());
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

}  // namespace laufey_common
