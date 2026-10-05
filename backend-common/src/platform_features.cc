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
  // KWallet asks for its own unlock, and Chromium picks it on Plasma.
  if (f.kwallet)
    return false;
  switch (f.secret_service) {
    case SecretServiceState::kLocked:
    case SecretServiceState::kActivatable:
      // Reaching the key may need an unlock prompt (a freshly started
      // gnome-keyring opens with its login keyring locked). Fine when a
      // person can answer it; a hang otherwise.
      return !f.secret_prompter;
    case SecretServiceState::kAvailable:
    case SecretServiceState::kNotApplicable:
    // No service (or no bus): Chromium finds none and falls back to basic by
    // itself, without waiting. Left to it.
    case SecretServiceState::kAbsent:
    case SecretServiceState::kNoSessionBus:
      return false;
  }
  return false;
}

std::string BasicPasswordStoreReason(const PlatformFeatures& f) {
  std::string session =
      f.session_type.empty() ? std::string("unknown") : f.session_type;
  switch (f.secret_service) {
    case SecretServiceState::kLocked:
      return "the Secret Service's default keyring is locked and no one can "
             "answer its unlock prompt in this " +
             session + " session";
    case SecretServiceState::kActivatable:
      return "the Secret Service is not running, and once started it may ask "
             "for an unlock no one can answer in this " +
             session + " session";
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

std::string NotificationUnavailableReason(const PlatformFeatures& f) {
  if (f.os != "linux" || !f.notification_server.empty())
    return "";
  if (!f.session_bus)
    return "no D-Bus session bus";
  if (f.notification_activatable) {
    return "no notification server is running; D-Bus can start one for "
           "org.freedesktop.Notifications (it is tried when notifications "
           "are first used)";
  }
  return "no notification server: nothing owns org.freedesktop.Notifications "
         "on the session bus" +
         DesktopSuffix(f);
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
  out += ",\"notificationServer\":" + StringOrNull(f.notification_server);
  out += ",\"notificationReason\":" +
         StringOrNull(NotificationUnavailableReason(f));
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

const char kPasswordStoreMarkerName[] = "laufey-password-store";

namespace {
std::string MarkerPath(const std::string& dir) {
#ifdef _WIN32
  return dir + "\\" + kPasswordStoreMarkerName;
#else
  return dir + "/" + kPasswordStoreMarkerName;
#endif
}
}  // namespace

std::string ReadPasswordStoreMarker(const std::string& root_cache_dir) {
  if (root_cache_dir.empty())
    return "";
  FILE* file = std::fopen(MarkerPath(root_cache_dir).c_str(), "rb");
  if (!file)
    return "";
  char buf[16] = {0};
  size_t n = std::fread(buf, 1, sizeof(buf) - 1, file);
  std::fclose(file);
  std::string value(buf, n);
  while (!value.empty() && (value.back() == '\n' || value.back() == '\r' ||
                            value.back() == ' '))
    value.pop_back();
  return value == "basic" || value == "os" ? value : "";
}

bool WritePasswordStoreMarker(const std::string& root_cache_dir,
                              const std::string& store) {
  if (root_cache_dir.empty() || (store != "basic" && store != "os"))
    return false;
  FILE* file = std::fopen(MarkerPath(root_cache_dir).c_str(), "wb");
  if (!file)
    return false;
  std::string line = store + "\n";
  bool ok = std::fwrite(line.data(), 1, line.size(), file) == line.size();
  return std::fclose(file) == 0 && ok;
}

PasswordStoreChoice ChoosePasswordStore(
    const std::string* explicit_store,
    const std::string& marker,
    const std::function<PlatformFeatures()>& probe) {
  PasswordStoreChoice choice;
  if (explicit_store) {
    // As given on the command line (Chromium reads the switch itself).
    choice.store = *explicit_store == "basic" ? "basic" : "os";
    choice.source = "explicit";
    choice.record = marker != choice.store;
    return choice;
  }
  if (!marker.empty()) {
    choice.store = marker;
    choice.source = "profile";
    choice.append_basic = marker == "basic";
    return choice;
  }
  PlatformFeatures f = probe();
  bool basic = NeedsBasicPasswordStore(f);
  choice.store = basic ? "basic" : "os";
  choice.source = "probe";
  choice.append_basic = basic;
  choice.record = true;
  if (basic)
    choice.reason = BasicPasswordStoreReason(f);
  return choice;
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

char* TrayUnavailableReasonForAbi() {
  PlatformFeatures f;
  ProbeTray(&f);
  std::string reason = TrayUnavailableReason(f);
  if (reason.empty())
    return nullptr;
  char* out = static_cast<char*>(std::malloc(reason.size() + 1));
  if (out)
    std::memcpy(out, reason.c_str(), reason.size() + 1);
  return out;
}

#if !defined(__linux__) || defined(__ANDROID__)

// Nothing here changes while the app runs.
void SetPlatformFeaturesChangedHandler(void (*)(void*), void*) {}

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

int XEmbedProbeCountForTesting() {
  return 0;
}

#endif

}  // namespace laufey_common
