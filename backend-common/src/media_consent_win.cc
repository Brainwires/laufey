// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Windows' privacy switches for the microphone and camera, read from the
// registry (see laufey_media_consent.h). Read-only: laufey never changes
// them.

#include "laufey_media_consent.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace laufey_common {

namespace {

constexpr wchar_t kConsentStore[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\"
    L"ConsentStore\\";

// `root\subkey`'s "Value" string, ASCII ("Allow" / "Deny"); empty when the
// key or value is missing, is not a string, or is unusually long.
std::string ReadValue(HKEY root, const std::wstring& subkey) {
  wchar_t buf[32] = {0};
  DWORD size = sizeof(buf);
  LSTATUS status = RegGetValueW(root, subkey.c_str(), L"Value", RRF_RT_REG_SZ,
                                nullptr, buf, &size);
  if (status != ERROR_SUCCESS)
    return "";
  std::string out;
  for (const wchar_t* p = buf; *p; ++p) {
    if (*p > 0x7f)
      return "";
    out += static_cast<char>(*p);
  }
  return out;
}

}  // namespace

OsMediaConsentState ProbeOsMediaConsent(uint32_t kind) {
  const char* device = ConsentStoreDevice(kind);
  if (!device)
    return {};
  std::wstring key = kConsentStore;
  for (const char* p = device; *p; ++p)
    key += static_cast<wchar_t>(*p);
  ConsentStoreSwitches switches;
  switches.machine = ReadValue(HKEY_LOCAL_MACHINE, key);
  switches.user = ReadValue(HKEY_CURRENT_USER, key);
  switches.desktop_apps = ReadValue(HKEY_CURRENT_USER, key + L"\\NonPackaged");
  return ConsentFromSwitches(switches, kind);
}

}  // namespace laufey_common
