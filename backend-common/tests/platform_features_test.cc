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

  // macOS and Windows: the OS keystore.
  PlatformFeatures mac;
  mac.os = "macos";
  EXPECT(mac.secret_service == SecretServiceState::kNotApplicable);
  EXPECT(!NeedsBasicPasswordStore(mac));
  EXPECT(TrayAvailable(mac));
  EXPECT(TrayUnavailableReason(mac).empty());

  // --- The cookie store is sticky per profile ----------------------------
  {
    int probes = 0;
    PlatformFeatures locked = Linux();
    locked.secret_service = SecretServiceState::kLocked;
    locked.secret_prompter = false;  // headless: basic
    auto probe_locked = [&] {
      ++probes;
      return locked;
    };
    auto probe_unlocked = [&] {
      ++probes;
      return Linux();
    };
    // A fresh profile: the probe decides, and the choice is recorded.
    PasswordStoreChoice c = ChoosePasswordStore(nullptr, "", probe_locked);
    EXPECT(c.store == "basic" && c.append_basic && c.record);
    EXPECT(c.source == "probe" && Contains(c.reason, "locked"));
    c = ChoosePasswordStore(nullptr, "", probe_unlocked);
    EXPECT(c.store == "os" && !c.append_basic && c.record);
    EXPECT(probes == 2);
    // A profile that chose "os" stays "os" on a headless launch (its
    // cookies use the OS key), and one that chose "basic" stays basic in a
    // desktop session; neither probes.
    c = ChoosePasswordStore(nullptr, "os", probe_locked);
    EXPECT(c.store == "os" && !c.append_basic && !c.record);
    EXPECT(c.source == "profile");
    c = ChoosePasswordStore(nullptr, "basic", probe_unlocked);
    EXPECT(c.store == "basic" && c.append_basic && !c.record);
    EXPECT(probes == 2);
    // An explicit --password-store wins, is not appended again (Chromium
    // reads it), and is recorded when it changes the profile's choice.
    std::string explicit_store = "gnome-libsecret";
    c = ChoosePasswordStore(&explicit_store, "basic", probe_locked);
    EXPECT(c.store == "os" && !c.append_basic && c.record);
    EXPECT(c.source == "explicit");
    explicit_store = "basic";
    c = ChoosePasswordStore(&explicit_store, "basic", probe_unlocked);
    EXPECT(c.store == "basic" && !c.append_basic && !c.record);
    EXPECT(probes == 2);

    // The marker file round trip.
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("laufey-pf-marker-" + std::to_string(std::rand()));
    std::filesystem::create_directories(dir);
    EXPECT(ReadPasswordStoreMarker(dir.string()).empty());
    EXPECT(WritePasswordStoreMarker(dir.string(), "basic"));
    EXPECT(ReadPasswordStoreMarker(dir.string()) == "basic");
    EXPECT(WritePasswordStoreMarker(dir.string(), "os"));
    EXPECT(ReadPasswordStoreMarker(dir.string()) == "os");
    EXPECT(!WritePasswordStoreMarker(dir.string(), "kwallet"));
    EXPECT(!WritePasswordStoreMarker("", "basic"));  // a profile in memory
    EXPECT(ReadPasswordStoreMarker("").empty());
    // Anything else in the file counts as no choice.
    {
      FILE* file =
          std::fopen((dir / kPasswordStoreMarkerName).string().c_str(), "wb");
      EXPECT(file);
      std::fputs("gnome-libsecret\n", file);
      std::fclose(file);
    }
    EXPECT(ReadPasswordStoreMarker(dir.string()).empty());
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
  EXPECT(Contains(reason, "org.kde.StatusNotifierWatcher"));
  EXPECT(Contains(reason, "XDG_CURRENT_DESKTOP=GNOME"));
  EXPECT(Contains(reason, "AppIndicator extension"));
  EXPECT(!Contains(reason, "XEmbed"));  // a Wayland session has none
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
