// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_platform_features.h, the decisions made from a probe (no bus): when
// Chromium's cookie store must not wait for the Secret Service, when a tray
// icon can be seen and why not, and the JSON platform_features hands out.
// Plain asserts, no framework.

#include "laufey_platform_features.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using namespace laufey_common;

static bool Contains(const std::string& s, const std::string& needle) {
  return s.find(needle) != std::string::npos;
}

static PlatformFeatures Linux() {
  PlatformFeatures f;
  f.os = "linux";
  f.session_type = "wayland";
  f.session_bus = true;
  f.tray_watcher = true;
  f.tray_clicks = false;
  f.secret_service = SecretServiceState::kAvailable;
  f.secret_prompter = true;
  return f;
}

int main() {
#ifndef _WIN32
  // The ABI string below runs the real probe: keep it off this machine's
  // session (no bus, no display), so it answers at once.
  unsetenv("DBUS_SESSION_BUS_ADDRESS");
  unsetenv("XDG_RUNTIME_DIR");
  unsetenv("DISPLAY");
  unsetenv("WAYLAND_DISPLAY");
#endif

  // --- The cookie store ------------------------------------------------------
  PlatformFeatures f = Linux();
  EXPECT(!NeedsBasicPasswordStore(f));
  EXPECT(BasicPasswordStoreReason(f).empty());

  // No provider, or no bus: Chromium finds no keystore and falls back to
  // basic by itself, without waiting. Left to it.
  f.secret_service = SecretServiceState::kAbsent;
  f.secret_prompter = false;
  EXPECT(!NeedsBasicPasswordStore(f));
  f.secret_service = SecretServiceState::kNoSessionBus;
  EXPECT(!NeedsBasicPasswordStore(f));
  f.secret_prompter = true;

  // Locked (or not yet started): fine while someone can answer the prompt,
  // a hang when no one can.
  for (SecretServiceState s :
       {SecretServiceState::kLocked, SecretServiceState::kActivatable}) {
    f.secret_service = s;
    f.secret_prompter = true;
    EXPECT(!NeedsBasicPasswordStore(f));
    f.secret_prompter = false;
    f.session_type = "tty";
    EXPECT(NeedsBasicPasswordStore(f));
    EXPECT(Contains(BasicPasswordStoreReason(f), "tty session"));
    // KWallet (Chromium's store on Plasma) asks for its own unlock.
    f.kwallet = true;
    EXPECT(!NeedsBasicPasswordStore(f));
    f.kwallet = false;
    f.session_type = "wayland";
  }

  // Whether Chromium's cookie store would use KWallet: its own rule over
  // XDG_CURRENT_DESKTOP (the first desktop it knows), then DESKTOP_SESSION
  // and the older variables.
  {
    auto picks = [](std::map<std::string, std::string> vars) {
      return ChromiumPicksKWallet([&vars](const char* name) {
        auto it = vars.find(name);
        return it == vars.end() ? std::string() : it->second;
      });
    };
    EXPECT(picks({{"XDG_CURRENT_DESKTOP", "KDE"}}));
    EXPECT(
        picks({{"XDG_CURRENT_DESKTOP", "KDE"}, {"KDE_SESSION_VERSION", "6"}}));
    EXPECT(!picks({{"XDG_CURRENT_DESKTOP", "GNOME"}}));
    EXPECT(!picks({{"XDG_CURRENT_DESKTOP", "ubuntu:GNOME"}}));
    // The first desktop Chromium knows decides; unknown ones are skipped.
    EXPECT(!picks({{"XDG_CURRENT_DESKTOP", "GNOME:KDE"}}));
    EXPECT(picks({{"XDG_CURRENT_DESKTOP", "Hyprland: KDE :GNOME"}}));
    EXPECT(!picks({{"XDG_CURRENT_DESKTOP", "sway"}}));
    EXPECT(!picks({{"XDG_CURRENT_DESKTOP", "KDE-ish"}}));
    EXPECT(!picks({}));
    // Nothing it knows in XDG_CURRENT_DESKTOP: DESKTOP_SESSION.
    EXPECT(picks({{"XDG_CURRENT_DESKTOP", "sway"},
                  {"DESKTOP_SESSION", "plasma"},
                  {"KDE_FULL_SESSION", "true"},
                  {"KDE_SESSION_VERSION", "5"}}));
    EXPECT(picks({{"DESKTOP_SESSION", "kde-plasma"}}));
    EXPECT(picks({{"DESKTOP_SESSION", "kde4"}}));
    EXPECT(picks({{"DESKTOP_SESSION", "kde"}, {"KDE_SESSION_VERSION", "5"}}));
    EXPECT(!picks({{"DESKTOP_SESSION", "kde"}}));  // KDE 3: basic
    EXPECT(!picks({{"DESKTOP_SESSION", "gnome"}, {"KDE_FULL_SESSION", "1"}}));
    EXPECT(!picks({{"GNOME_DESKTOP_SESSION_ID", "x"},
                   {"KDE_FULL_SESSION", "true"},
                   {"KDE_SESSION_VERSION", "5"}}));
    EXPECT(picks({{"KDE_FULL_SESSION", "true"}, {"KDE_SESSION_VERSION", "5"}}));
    EXPECT(!picks({{"KDE_FULL_SESSION", "true"}}));
  }

  // macOS and Windows: the OS keystore.
  PlatformFeatures mac;
  mac.os = "macos";
  EXPECT(mac.secret_service == SecretServiceState::kNotApplicable);
  EXPECT(!NeedsBasicPasswordStore(mac));
  EXPECT(TrayAvailable(mac));
  EXPECT(TrayUnavailableReason(mac).empty());

  // --- The OS key is sticky per profile; basic is never recorded ---------
  {
    int probes = 0;
    PlatformFeatures locked = Linux();
    locked.secret_service = SecretServiceState::kLocked;
    locked.secret_prompter = false;  // headless: basic
    PlatformFeatures activatable = locked;
    activatable.secret_service = SecretServiceState::kActivatable;
    auto probe_locked = [&] {
      ++probes;
      return locked;
    };
    auto probe_activatable = [&] {
      ++probes;
      return activatable;
    };
    auto probe_unlocked = [&] {
      ++probes;
      return Linux();
    };
    // A fresh profile: the probe decides. Basic is not recorded (basic ->
    // os later is lossless: Chromium reads v10 cookies under os); os is.
    PasswordStoreChoice c = ChoosePasswordStore(nullptr, "", probe_locked);
    EXPECT(c.store == "basic" && c.append_basic && !c.record);
    EXPECT(c.source == "probe" && Contains(c.reason, "locked"));
    EXPECT(!c.os_unavailable);
    c = ChoosePasswordStore(nullptr, "", probe_unlocked);
    EXPECT(c.store == "os" && !c.append_basic && c.record);
    EXPECT(c.source == "probe" && !c.os_unavailable);
    EXPECT(probes == 2);
    // An "os" profile in a session that can reach the key: os, nothing
    // rewritten.
    c = ChoosePasswordStore(nullptr, "os", probe_unlocked);
    EXPECT(c.store == "os" && !c.append_basic && !c.record);
    EXPECT(c.source == "profile" && !c.os_unavailable);
    // An "os" profile whose keyring is locked, or not running, with no one
    // to answer: basic for this launch only, the marker unchanged, and the
    // warning (os_unavailable) with the reason.
    c = ChoosePasswordStore(nullptr, "os", probe_locked);
    EXPECT(c.store == "basic" && c.append_basic && !c.record);
    EXPECT(c.source == "profile" && c.os_unavailable);
    EXPECT(Contains(c.reason, "locked"));
    c = ChoosePasswordStore(nullptr, "os", probe_activatable);
    EXPECT(c.store == "basic" && c.append_basic && !c.record);
    EXPECT(c.os_unavailable && Contains(c.reason, "not running"));
    EXPECT(probes == 5);
    // An explicit --password-store wins and is not appended again (Chromium
    // reads it). An OS store is recorded when the profile lacks it; basic
    // never is, and leaves an "os" marker alone. The probe doesn't run.
    std::string explicit_store = "gnome-libsecret";
    c = ChoosePasswordStore(&explicit_store, "", probe_locked);
    EXPECT(c.store == "os" && !c.append_basic && c.record);
    EXPECT(c.source == "explicit");
    c = ChoosePasswordStore(&explicit_store, "os", probe_locked);
    EXPECT(c.store == "os" && !c.record);
    explicit_store = "basic";
    c = ChoosePasswordStore(&explicit_store, "os", probe_unlocked);
    EXPECT(c.store == "basic" && !c.append_basic && !c.record);
    c = ChoosePasswordStore(&explicit_store, "", probe_unlocked);
    EXPECT(c.store == "basic" && !c.record);
    EXPECT(probes == 5);

    // The marker file round trip: only "os" is written, atomically.
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("laufey-pf-marker-" + std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    EXPECT(ReadPasswordStoreMarker(dir.string()).empty());
    EXPECT(!WritePasswordStoreMarker(dir.string(), "basic"));
    EXPECT(ReadPasswordStoreMarker(dir.string()).empty());
    EXPECT(WritePasswordStoreMarker(dir.string(), "os"));
    EXPECT(ReadPasswordStoreMarker(dir.string()) == "os");
    EXPECT(WritePasswordStoreMarker(dir.string(), "os"));  // replaces it
    EXPECT(ReadPasswordStoreMarker(dir.string()) == "os");
    EXPECT(!WritePasswordStoreMarker(dir.string(), "kwallet"));
    EXPECT(!WritePasswordStoreMarker("", "os"));  // a profile in memory
    EXPECT(ReadPasswordStoreMarker("").empty());
    // Written through a temporary file renamed over it: nothing else is
    // left in the directory.
    int entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      EXPECT(entry.path().filename() == kPasswordStoreMarkerName);
      ++entries;
    }
    EXPECT(entries == 1);
    // A directory that can't be written to: false, and nothing half done.
    EXPECT(!WritePasswordStoreMarker((dir / "missing").string(), "os"));
    // Anything else in the file counts as no choice (an older "basic"
    // marker included).
    for (const char* other : {"gnome-libsecret\n", "basic\n"}) {
      FILE* file =
          std::fopen((dir / kPasswordStoreMarkerName).string().c_str(), "wb");
      EXPECT(file);
      std::fputs(other, file);
      std::fclose(file);
      EXPECT(ReadPasswordStoreMarker(dir.string()).empty());
    }
    std::filesystem::remove_all(dir);
  }

  // --- The tray
  // ----------------------------------------------------------------
  f = Linux();
  EXPECT(TrayAvailable(f));
  EXPECT(TrayUnavailableReason(f).empty());
  // Stock GNOME: no watcher, no XEmbed tray.
  f.tray_watcher = false;
  f.desktop_hint = "GNOME";
  EXPECT(!TrayAvailable(f));
  std::string reason = TrayUnavailableReason(f);
  EXPECT(reason ==
         "no tray host (StatusNotifierWatcher) on this session; some desktops "
         "need an extension (XDG_CURRENT_DESKTOP=GNOME; GNOME shows tray "
         "icons only with the AppIndicator extension enabled)");
  EXPECT(!Contains(reason, "XEmbed"));  // a Wayland session has none
  // No desktop is named outside the hint's suffix, which is the reason's
  // last part: cut there (a runtime that may not show the hint), the
  // wording is neutral.
  EXPECT(reason.substr(0, reason.find(" (XDG_CURRENT_DESKTOP=")) ==
         "no tray host (StatusNotifierWatcher) on this session; some desktops "
         "need an extension");
  f.desktop_hint = "ubuntu:GNOME";
  EXPECT(Contains(TrayUnavailableReason(f), "AppIndicator extension"));
  f.desktop_hint = "sway";
  EXPECT(TrayUnavailableReason(f) ==
         "no tray host (StatusNotifierWatcher) on this session; some desktops "
         "need an extension (XDG_CURRENT_DESKTOP=sway)");
  f.desktop_hint = "";
  EXPECT(TrayUnavailableReason(f) ==
         "no tray host (StatusNotifierWatcher) on this session; some desktops "
         "need an extension");
  // X11: an XEmbed tray is a host too (appindicator falls back to it).
  f.session_type = "x11";
  EXPECT(Contains(TrayUnavailableReason(f), "XEmbed"));
  f.tray_xembed = true;
  EXPECT(TrayAvailable(f));
  // The library is required either way.
  f.tray_library = false;
  EXPECT(!TrayAvailable(f));
  EXPECT(Contains(TrayUnavailableReason(f), "libayatana-appindicator3"));

  // --- JSON
  // ----------------------------------------------------------------------
  f = Linux();
  f.desktop_hint = "KDE";
  f.portal_versions = {{"Notification", 2}, {"Settings", 2}};
  f.cookie_encryption = "os";
  f.notification_server = "Plasma";
  std::string json = PlatformFeaturesToJson(f);
  EXPECT(json ==
         "{\"os\":\"linux\",\"sessionType\":\"wayland\","
         "\"desktopHint\":\"KDE\",\"sessionBus\":true,\"trayHost\":true,"
         "\"trayReason\":null,\"trayClicks\":false,\"trayTooltip\":true,"
         "\"secretService\":\"available\",\"secretServicePrompt\":true,"
         "\"notificationServer\":\"Plasma\",\"notificationReason\":null,"
         "\"portalVersions\":{\"Notification\":2,\"Settings\":2},"
         "\"cookieEncryption\":\"os\"}");

  // No notification server (Sway with no daemon): the Notification portal
  // is no proof; the reason says what is missing.
  {
    PlatformFeatures n = f;
    n.notification_server.clear();
    std::string reason = NotificationUnavailableReason(n);
    EXPECT(Contains(reason, "nothing owns org.freedesktop.Notifications"));
    EXPECT(Contains(PlatformFeaturesToJson(n),
                    "\"notificationServer\":null,\"notificationReason\":\"no "
                    "notification server"));
    n.notification_activatable = true;
    EXPECT(Contains(NotificationUnavailableReason(n), "can start one"));
    n.session_bus = false;
    EXPECT(NotificationUnavailableReason(n) == "no D-Bus session bus");
    EXPECT(NotificationUnavailableReason(mac).empty());
  }

  f.tray_watcher = false;
  f.desktop_hint = "a\"b\\c\n";
  f.secret_service = SecretServiceState::kLocked;
  f.portal_versions.clear();
  f.cookie_encryption.clear();
  json = PlatformFeaturesToJson(f);
  EXPECT(Contains(json, "\"desktopHint\":\"a\\\"b\\\\c\\n\""));
  EXPECT(Contains(json, "\"trayHost\":false"));
  EXPECT(Contains(json, "\"trayReason\":\"no tray host"));
  EXPECT(Contains(json, "\"secretService\":\"locked\""));
  EXPECT(Contains(json, "\"portalVersions\":{}"));
  EXPECT(Contains(json, "\"cookieEncryption\":null"));

  json = PlatformFeaturesToJson(mac);
  EXPECT(Contains(json, "\"sessionType\":null"));
  EXPECT(Contains(json, "\"notificationServer\":null,\"notificationReason\":null"));
  EXPECT(Contains(json, "\"secretService\":\"os\""));

  // The ABI string carries the backend's cookie-store decision.
  SetCookieEncryption("basic");
  char* abi = PlatformFeaturesJsonForAbi();
  EXPECT(abi && Contains(abi, "\"cookieEncryption\":\"basic\""));
  std::free(abi);
  SetCookieEncryption(nullptr);
  abi = PlatformFeaturesJsonForAbi();
  EXPECT(abi && Contains(abi, "\"cookieEncryption\":null"));
  std::free(abi);

  std::printf("laufey_platform_features_test: OK\n");
  return 0;
}
