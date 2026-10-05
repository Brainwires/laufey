// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_platform_features.h, the decisions made from a probe (no bus): when
// Chromium's cookie store must not wait for the Secret Service, when a tray
// icon can be seen and why not, and the JSON platform_features hands out.
// Plain asserts, no framework.

#include "laufey_platform_features.h"

#include <cstdio>
#include <cstdlib>
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

  // No provider, or no bus: nothing to wait for, so no OS key either.
  f.secret_service = SecretServiceState::kAbsent;
  EXPECT(NeedsBasicPasswordStore(f));
  EXPECT(Contains(BasicPasswordStoreReason(f), "org.freedesktop.secrets"));
  f.secret_service = SecretServiceState::kNoSessionBus;
  EXPECT(NeedsBasicPasswordStore(f));
  EXPECT(Contains(BasicPasswordStoreReason(f), "session bus"));

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
    f.session_type = "wayland";
  }

  // macOS and Windows: the OS keystore.
  PlatformFeatures mac;
  mac.os = "macos";
  EXPECT(mac.secret_service == SecretServiceState::kNotApplicable);
  EXPECT(!NeedsBasicPasswordStore(mac));
  EXPECT(TrayAvailable(mac));
  EXPECT(TrayUnavailableReason(mac).empty());

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
  std::string json = PlatformFeaturesToJson(f);
  EXPECT(json ==
         "{\"os\":\"linux\",\"sessionType\":\"wayland\","
         "\"desktopHint\":\"KDE\",\"sessionBus\":true,\"trayHost\":true,"
         "\"trayReason\":null,\"trayClicks\":false,\"trayTooltip\":true,"
         "\"secretService\":\"available\",\"secretServicePrompt\":true,"
         "\"portalVersions\":{\"Notification\":2,\"Settings\":2},"
         "\"cookieEncryption\":\"os\"}");

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
