// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Platform features (API 45), the portable part: the decisions made from a
// probe, its JSON, and the static answer on macOS and Windows. The Linux
// probe is platform_features_linux.cc. See laufey_platform_features.h.

#include "laufey_platform_features.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace laufey_common {

namespace {

std::mutex& CookieMutex() {
  static std::mutex m;
  return m;
}
std::string& CookieEncryption() {
  static std::string value;
  return value;
}
std::string Quote(const std::string& s) {
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += "\"";
  return out;
}

std::string StringOrNull(const std::string& s) {
  return s.empty() ? "null" : Quote(s);
}

const char* Bool(bool b) {
  return b ? "true" : "false";
}

// "on GNOME" style suffix from the desktop hint, for wording only.
std::string DesktopSuffix(const PlatformFeatures& f) {
  if (f.desktop_hint.empty())
    return "";
  return " (XDG_CURRENT_DESKTOP=" + f.desktop_hint + ")";
}

}  // namespace

const char* SecretServiceStateName(SecretServiceState state) {
  switch (state) {
    case SecretServiceState::kAvailable:
      return "available";
    case SecretServiceState::kLocked:
      return "locked";
    case SecretServiceState::kActivatable:
      return "activatable";
    case SecretServiceState::kAbsent:
      return "absent";
    case SecretServiceState::kNoSessionBus:
      return "no-session-bus";
    case SecretServiceState::kNotApplicable:
      return "os";
  }
  return "absent";
}

bool NeedsBasicPasswordStore(const PlatformFeatures& f) {
  switch (f.secret_service) {
    case SecretServiceState::kAvailable:
    case SecretServiceState::kNotApplicable:
      return false;
    case SecretServiceState::kAbsent:
    case SecretServiceState::kNoSessionBus:
      return true;
    case SecretServiceState::kLocked:
    case SecretServiceState::kActivatable:
      // Reaching the key may need an unlock prompt (a freshly started
      // gnome-keyring opens with its login keyring locked). Fine when a
      // person can answer it; a hang otherwise.
      return !f.secret_prompter;
  }
  return true;
}

std::string BasicPasswordStoreReason(const PlatformFeatures& f) {
  switch (f.secret_service) {
    case SecretServiceState::kAbsent:
      return "no Secret Service (org.freedesktop.secrets) on the session bus";
    case SecretServiceState::kNoSessionBus:
      return "no D-Bus session bus";
    case SecretServiceState::kLocked:
      return "the Secret Service's default keyring is locked and no one can "
             "answer its unlock prompt in this " +
             (f.session_type.empty() ? std::string("unknown")
                                     : f.session_type) +
             " session";
    case SecretServiceState::kActivatable:
      return "the Secret Service is not running, and once started it may ask "
             "for an unlock no one can answer in this " +
             (f.session_type.empty() ? std::string("unknown")
                                     : f.session_type) +
             " session";
    default:
      return "";
  }
}

bool TrayAvailable(const PlatformFeatures& f) {
  if (f.os != "linux")
    return true;
  return f.tray_library && (f.tray_watcher || f.tray_xembed);
}

std::string TrayUnavailableReason(const PlatformFeatures& f) {
  if (TrayAvailable(f))
    return "";
  if (!f.tray_library) {
    return "no tray library: install libayatana-appindicator3 (or "
           "libappindicator3)";
  }
  std::string reason =
      "no tray host: nothing owns org.kde.StatusNotifierWatcher on the "
      "session bus";
  if (f.session_type == "x11")
    reason += " and no XEmbed system tray runs";
  reason += DesktopSuffix(f);
  reason +=
      "; GNOME shows tray icons only with the AppIndicator extension enabled";
  return reason;
}

std::string PlatformFeaturesToJson(const PlatformFeatures& f) {
  std::string out = "{";
  out += "\"os\":" + Quote(f.os);
  out += ",\"sessionType\":" + StringOrNull(f.session_type);
  out += ",\"desktopHint\":" + StringOrNull(f.desktop_hint);
  out += std::string(",\"sessionBus\":") + Bool(f.session_bus);
  out += std::string(",\"trayHost\":") + Bool(TrayAvailable(f));
  out += ",\"trayReason\":" + StringOrNull(TrayUnavailableReason(f));
  out += std::string(",\"trayClicks\":") + Bool(f.tray_clicks);
  out += std::string(",\"trayTooltip\":") + Bool(f.tray_tooltip);
  out += std::string(",\"secretService\":") +
         Quote(SecretServiceStateName(f.secret_service));
  out += std::string(",\"secretServicePrompt\":") + Bool(f.secret_prompter);
  out += ",\"portalVersions\":{";
  bool first = true;
  for (const auto& [iface, version] : f.portal_versions) {
    if (!first)
      out += ",";
    first = false;
    out += Quote(iface) + ":" + std::to_string(version);
  }
  out += "}";
  out += ",\"cookieEncryption\":" + StringOrNull(f.cookie_encryption);
  out += "}";
  return out;
}

void SetCookieEncryption(const char* value) {
  std::lock_guard<std::mutex> lock(CookieMutex());
  CookieEncryption() = value ? value : "";
}

char* PlatformFeaturesJsonForAbi() {
  PlatformFeatures f = ProbePlatformFeatures();
  {
    std::lock_guard<std::mutex> lock(CookieMutex());
    f.cookie_encryption = CookieEncryption();
  }
  std::string json = PlatformFeaturesToJson(f);
  char* out = static_cast<char*>(std::malloc(json.size() + 1));
  if (out)
    std::memcpy(out, json.c_str(), json.size() + 1);
  return out;
}

#if !defined(__linux__) || defined(__ANDROID__)

// macOS and Windows: the OS keystore never blocks on a prompt the app can't
// answer, the tray always has a host, and there are no portals.
void ProbeSecretService(PlatformFeatures* out) {
  out->secret_service = SecretServiceState::kNotApplicable;
  out->secret_prompter = true;
}

void ProbeTray(PlatformFeatures* out) {
  *out = ProbePlatformFeatures();
}

PlatformFeatures ProbePlatformFeatures() {
  PlatformFeatures f;
#if defined(__APPLE__)
  f.os = "macos";
#elif defined(_WIN32)
  f.os = "windows";
#else
  f.os = "unknown";
#endif
  ProbeSecretService(&f);
  return f;
}

void ResetPlatformFeaturesForTesting() {}

#endif

}  // namespace laufey_common
