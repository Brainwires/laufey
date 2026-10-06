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
#ifdef _WIN32
#include <process.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
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
std::string& CookieEncryptionWait() {
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

// The desktop hint in a reason: " (XDG_CURRENT_DESKTOP=<hint><more>)",
// always the LAST part of the reason, so a runtime that may not show the
// hint (deno without env access) cuts the reason there. `more` (wording
// that names the desktop) goes inside it for the same reason. Empty without
// a hint.
std::string DesktopSuffix(const PlatformFeatures& f,
                          const std::string& more = "") {
  if (f.desktop_hint.empty())
    return "";
  return " (XDG_CURRENT_DESKTOP=" + f.desktop_hint + more + ")";
}

// XDG_CURRENT_DESKTOP lists `name` (it is a colon-separated list).
bool HintNames(const PlatformFeatures& f, const char* name) {
  std::string list = ":" + f.desktop_hint + ":";
  return list.find(std::string(":") + name + ":") != std::string::npos;
}

}  // namespace

const char* KWalletStateName(KWalletState state) {
  switch (state) {
    case KWalletState::kNotUsed:
      return nullptr;
    case KWalletState::kOpen:
      return "open";
    case KWalletState::kClosed:
      return "closed";
    case KWalletState::kDisabled:
      return "disabled";
    case KWalletState::kNotRunning:
      return "not-running";
  }
  return nullptr;
}

const char* ProfileCookieKeysName(ProfileCookieKeys keys) {
  switch (keys) {
    case ProfileCookieKeys::kNone:
      return "none";
    case ProfileCookieKeys::kOsKey:
      return "os-key";
    case ProfileCookieKeys::kUnknown:
      return "unknown";
  }
  return "unknown";
}

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

bool ChromiumPicksKWallet(const std::function<const char*(const char*)>& env) {
  // base::nix::GetDesktopEnvironment: XDG_CURRENT_DESKTOP's values in
  // priority order; the first one Chromium knows decides. os_crypt's
  // SelectBackend maps KDE3 / KDE4 / KDE5 / KDE6 to KWallet.
  static const char* const kKnown[] = {
      "Unity", "Deepin", "GNOME", "X-Cinnamon", "Pantheon",
      "XFCE",  "UKUI",   "LXQt",  "COSMIC",
  };
  auto value_of = [&env](const char* name) {
    const char* v = env(name);
    return std::string(v ? v : "");
  };
  // HasVar: set, even to "".
  auto has = [&env](const char* name) { return env(name) != nullptr; };
  std::string current = value_of("XDG_CURRENT_DESKTOP");
  size_t start = 0;
  while (start <= current.size()) {
    size_t end = current.find(':', start);
    if (end == std::string::npos)
      end = current.size();
    std::string value = current.substr(start, end - start);
    // TRIM_WHITESPACE
    size_t b = value.find_first_not_of(" \t\r\n");
    size_t e = value.find_last_not_of(" \t\r\n");
    value = b == std::string::npos ? "" : value.substr(b, e - b + 1);
    if (value == "KDE")
      return true;  // KDE4 / KDE5 / KDE6: KWallet
    for (const char* known : kKnown) {
      if (value == known)
        return false;
    }
    start = end + 1;
  }
  // DESKTOP_SESSION, then the older variables.
  std::string session = value_of("DESKTOP_SESSION");
  if (session == "kde4" || session == "kde-plasma" || session == "kde")
    return true;  // "kde" is KDE4 with KDE_SESSION_VERSION, else KDE3
  if (session == "deepin" || session == "gnome" || session == "mate" ||
      session.find("xfce") != std::string::npos || session == "xubuntu" ||
      session == "ukui")
    return false;
  if (has("GNOME_DESKTOP_SESSION_ID"))
    return false;
  // KDE_FULL_SESSION: KDE4 with KDE_SESSION_VERSION, else KDE3.
  return has("KDE_FULL_SESSION");
}

bool NeedsBasicPasswordStore(const PlatformFeatures& f) {
  // Chromium would use KWallet: only an open wallet answers (on Plasma its
  // request for a closed one is never answered, a person there or not).
  if (f.kwallet)
    return f.kwallet_state != KWalletState::kOpen;
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
  if (f.kwallet) {
    switch (f.kwallet_state) {
      case KWalletState::kClosed:
        return "the cookie store uses KWallet here, and its wallet is closed: "
               "a request for its key is never answered";
      case KWalletState::kDisabled:
        return "the cookie store uses KWallet here, and KWallet is disabled";
      case KWalletState::kNotRunning:
        return "the cookie store uses KWallet here, and kwalletd is not "
               "running: a request for its key may never be answered";
      case KWalletState::kOpen:
      case KWalletState::kNotUsed:
        return "";
    }
    return "";
  }
  if (!NeedsBasicPasswordStore(f))
    return "";
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
  // Neutral wording: no desktop is named outside the hint's suffix.
  std::string reason = "no tray host (StatusNotifierWatcher)";
  if (f.session_type == "x11")
    reason += " and no XEmbed system tray";
  reason += " on this session; some desktops need an extension";
  reason += DesktopSuffix(f, HintNames(f, "GNOME")
                                 ? "; GNOME shows tray icons only with the "
                                   "AppIndicator extension enabled"
                                 : "");
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
  const char* kwallet = KWalletStateName(f.kwallet_state);
  out += ",\"kwallet\":" + (kwallet ? Quote(kwallet) : std::string("null"));
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
  out += ",\"cookieEncryptionWait\":" + StringOrNull(f.cookie_encryption_wait);
  bool on_linux = f.os == "linux";
  out += ",\"notificationTransport\":" + StringOrNull(f.notification_transport);
  out += ",\"notificationColdStart\":" +
         std::string(on_linux ? Bool(f.notification_cold_start) : "null");
  out += ",\"notificationColdStartReason\":" +
         StringOrNull(on_linux && !f.notification_cold_start
                          ? f.notification_cold_start_reason
                          : std::string());
  out += ",\"notificationScheduleWhileClosed\":" +
         std::string(on_linux ? Bool(f.notification_schedule_while_closed)
                              : "null");
  out += ",\"notificationScheduleReason\":" +
         StringOrNull(on_linux && !f.notification_schedule_while_closed
                          ? f.notification_schedule_reason
                          : std::string());
  if (on_linux && f.notification_caps_known) {
    out += ",\"notificationServerCapabilities\":[";
    for (size_t i = 0; i < f.notification_server_caps.size(); ++i) {
      if (i)
        out += ",";
      out += Quote(f.notification_server_caps[i]);
    }
    out += "]";
  } else {
    out += ",\"notificationServerCapabilities\":null";
  }
  out += ",\"badge\":" + StringOrNull(f.badge);
  out += ",\"badgeReason\":" + StringOrNull(f.badge_reason);
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

int ProcessId() {
#ifdef _WIN32
  return _getpid();
#else
  return static_cast<int>(getpid());
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
  return value == "os" ? value : "";
}

bool WritePasswordStoreMarker(const std::string& root_cache_dir,
                              const std::string& store) {
  if (root_cache_dir.empty() || store != "os")
    return false;
  // A temporary file renamed over the marker: a crash (or a second
  // instance) never leaves a half-written one. Synced before the rename,
  // and the directory after it, so a power loss leaves the old marker or
  // the new one, never an empty file.
  std::string path = MarkerPath(root_cache_dir);
  std::string tmp = path + ".tmp" + std::to_string(ProcessId());
  FILE* file = std::fopen(tmp.c_str(), "wb");
  if (!file)
    return false;
  std::string line = store + "\n";
  bool ok = std::fwrite(line.data(), 1, line.size(), file) == line.size();
  ok = std::fflush(file) == 0 && ok;
#ifndef _WIN32
  ok = ok && fsync(fileno(file)) == 0;
#endif
  ok = std::fclose(file) == 0 && ok;
#ifdef _WIN32
  // rename() doesn't replace on Windows (the marker is used on Linux).
  if (ok)
    std::remove(path.c_str());
#endif
  ok = ok && std::rename(tmp.c_str(), path.c_str()) == 0;
  if (!ok) {
    std::remove(tmp.c_str());
    return false;
  }
#ifndef _WIN32
  int dir = open(root_cache_dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir >= 0) {
    fsync(dir);
    close(dir);
  }
#endif
  return true;
}

void RemoveStalePasswordStoreTemps(const std::string& root_cache_dir,
                                   int64_t launched_at) {
#ifndef _WIN32
  if (root_cache_dir.empty())
    return;
  DIR* dir = opendir(root_cache_dir.c_str());
  if (!dir)
    return;
  std::string prefix = std::string(kPasswordStoreMarkerName) + ".tmp";
  while (struct dirent* entry = readdir(dir)) {
    if (std::strncmp(entry->d_name, prefix.c_str(), prefix.size()) != 0)
      continue;
    std::string path = root_cache_dir + "/" + entry->d_name;
    struct stat st;
    if (lstat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
        static_cast<int64_t>(st.st_mtime) < launched_at)
      unlink(path.c_str());
  }
  closedir(dir);
#else
  (void)root_cache_dir;
  (void)launched_at;
#endif
}

PasswordStoreChoice ChoosePasswordStore(
    const std::string* explicit_store, const std::string& marker,
    ProfileCookieKeys cookies, const std::function<PlatformFeatures()>& probe) {
  PasswordStoreChoice choice;
  bool holds_os_cookies = cookies != ProfileCookieKeys::kNone;
  if (explicit_store) {
    // As given on the command line (Chromium reads the switch itself), even
    // when basic costs this profile its OS-key cookies: the person asked.
    choice.store = *explicit_store == "basic" ? "basic" : "os";
    choice.source = "explicit";
    choice.record = choice.store == "os" && marker != "os";
    choice.explicit_basic_deletes = choice.store == "basic" && holds_os_cookies;
    return choice;
  }
  PlatformFeatures f = probe();
  bool unreachable = NeedsBasicPasswordStore(f);
  if (unreachable)
    choice.reason = BasicPasswordStoreReason(f);
  if (unreachable && holds_os_cookies) {
    // Basic would make Chromium delete the OS-key cookies: keep the OS
    // store and wait for the key.
    choice.store = "os";
    choice.source = "cookies";
    choice.wait = true;
    choice.record = marker != "os";
    return choice;
  }
  choice.store = unreachable ? "basic" : "os";
  choice.append_basic = unreachable;
  choice.record = !unreachable && marker != "os";
  choice.source = !unreachable && marker == "os" ? "profile" : "probe";
  return choice;
}

std::string PasswordStoreWarning(const PasswordStoreChoice& choice,
                                 ProfileCookieKeys cookies,
                                 const std::string& detail) {
  std::string holds =
      cookies == ProfileCookieKeys::kUnknown
          ? "this profile's cookie database can't be read (" + detail +
                "), so it may hold cookies encrypted with the OS key"
          : std::string("this profile holds cookies encrypted with the OS key");
  if (choice.explicit_basic_deletes) {
    return "laufey: --password-store=basic was given, and " + holds +
           ": Chromium can't decrypt those under basic and deletes them "
           "(with every cookie of the same sites)";
  }
  if (choice.wait) {
    return "laufey: " + holds +
           ", so the cookie store keeps the OS key and waits until it is "
           "unlocked (requests that carry cookies wait with it); switching to "
           "--password-store=basic would delete those cookies: " +
           choice.reason;
  }
  if (choice.append_basic) {
    return "laufey: cookies are stored with --password-store=basic (not "
           "encrypted with an OS key): " +
           choice.reason;
  }
  return "";
}

void SetCookieEncryption(const char* value, const char* wait) {
  std::lock_guard<std::mutex> lock(CookieMutex());
  CookieEncryption() = value ? value : "";
  CookieEncryptionWait() = wait ? wait : "";
}

char* PlatformFeaturesJsonForAbi() {
  PlatformFeatures f = ProbePlatformFeatures();
  {
    std::lock_guard<std::mutex> lock(CookieMutex());
    f.cookie_encryption = CookieEncryption();
    f.cookie_encryption_wait = CookieEncryptionWait();
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

// The cookie-store choice is made on Linux only.
ProfileCookieKeys ReadProfileCookieKeys(const std::string&, std::string*) {
  return ProfileCookieKeys::kNone;
}

void ProbeTray(PlatformFeatures* out) {
  *out = ProbePlatformFeatures();
}

PlatformFeatures ProbePlatformFeatures() {
  PlatformFeatures f;
#if defined(__APPLE__)
  f.os = "macos";
  f.badge = "dock";
#elif defined(_WIN32)
  f.os = "windows";
  f.badge = "title";
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
