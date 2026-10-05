// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Platform features (API 45): what THIS session provides, probed instead of
// guessed from the desktop's name. Linux desktops differ in what they offer
// (a tray host, a secret service that can answer without a prompt, which
// xdg-desktop-portal interfaces at which version), and a feature the session
// lacks must be reported with a reason, never fail silently.
//
//   platform_features.cc        the portable part: the cookie-store decision,
//                               the tray reason and the JSON the C ABI hands
//                               out; the static answer on macOS and Windows
//   platform_features_linux.cc  the probe over the session bus (GDBus) and,
//                               for the XEmbed tray, the X server (XCB)
//
// XDG_CURRENT_DESKTOP is read only as a hint for wording a reason; nothing
// branches on it. See docs/platform-features.md.

#ifndef LAUFEY_PLATFORM_FEATURES_H_
#define LAUFEY_PLATFORM_FEATURES_H_

#include <cstdint>
#include <functional>
#include <map>
#include <string>

namespace laufey_common {

// Whether the Secret Service (org.freedesktop.secrets) can hand out a key.
enum class SecretServiceState {
  kAvailable,     // running, its default collection is unlocked
  kLocked,        // running, the default collection is locked or missing:
                  // using it means an unlock (or create) prompt
  kActivatable,   // not running, D-Bus can start it (its state is unknown
                  // until it runs; starting it is left to its first user)
  kAbsent,        // no provider on the session bus
  kNoSessionBus,  // no session bus to ask
  kNotApplicable  // not Linux: the OS keystore (Keychain, DPAPI) is always
                  // there and never blocks on a prompt
};

const char* SecretServiceStateName(SecretServiceState state);

// What the probe found. Strings are empty when not applicable.
struct PlatformFeatures {
  std::string os;            // "linux", "macos", "windows"
  std::string session_type;  // Linux: XDG_SESSION_TYPE ("wayland", "x11",
                             // "tty"), "unknown" when unset; never from
                             // $DISPLAY / $WAYLAND_DISPLAY
  std::string desktop_hint;  // XDG_CURRENT_DESKTOP, verbatim (a hint only)
  bool session_bus = false;  // Linux: a session bus answered

  // Tray icons: a StatusNotifierItem host (org.kde.StatusNotifierWatcher)
  // or, on X11, an XEmbed system tray (_NET_SYSTEM_TRAY_S<n>), and the
  // appindicator library laufey drives them with.
  bool tray_watcher = false;  // org.kde.StatusNotifierWatcher has an owner
  bool tray_xembed = false;   // X11 sessions only: a system tray selection
                              // owner (never probed in a Wayland session,
                              // whose $DISPLAY is Xwayland's)
  bool tray_library = true;   // libayatana-appindicator3 / libappindicator3
  bool tray_clicks = true;    // the icon reports left / double clicks
  bool tray_tooltip = true;   // set_tray_tooltip shows something

  SecretServiceState secret_service = SecretServiceState::kNotApplicable;
  // KWallet (org.kde.kwalletd5 / org.kde.kwalletd6) runs or can be started.
  // Chromium uses it instead of the Secret Service on Plasma, and it asks
  // for its own unlock.
  bool kwallet = false;
  // A person can answer an unlock prompt here: a graphical session
  // (XDG_SESSION_TYPE x11 / wayland with a display and, where logind can
  // say, an active x11 / wayland logind session), and the provider's
  // prompter (gnome-keyring's gcr-prompter) exists.
  bool secret_prompter = true;

  // Notifications: the name of the server that owns
  // org.freedesktop.Notifications right now (GetServerInformation; "unknown"
  // when it doesn't say), empty when nothing owns it; and whether D-Bus
  // could start one. The Notification portal's version is no proof: on
  // Sway with no daemon the portal still offers it, with nothing behind it.
  std::string notification_server;
  bool notification_activatable = false;

  // xdg-desktop-portal interface -> version ("Notification" -> 2). An
  // interface the portal lacks is absent from the map.
  std::map<std::string, uint32_t> portal_versions;

  // CEF only: "os" (OSCrypt keeps its key in the OS keystore) or "basic"
  // (--password-store=basic: the key is fixed, cookies are only obfuscated).
  // Empty on engines that don't encrypt with an OS key (WebKit, Winit).
  std::string cookie_encryption;
};

// --- The probe
// ----------------------------------------------------------------

// The secret-service part on its own, for a decision made before the event
// loop runs (CEF's command line). Synchronous, bounded by short D-Bus
// timeouts, never starts a service and never shows a prompt. Any thread.
void ProbeSecretService(PlatformFeatures* out);

// The tray part on its own (session facts plus the tray fields), for
// create_tray_icon: no portal calls. Any thread.
void ProbeTray(PlatformFeatures* out);

// The full probe. The session facts and the secret service are probed once
// per process; the tray host is re-read on every call (a watcher that
// appears late counts as soon as it does: the Linux probe follows the
// watcher's NameOwnerChanged); the portal versions are probed once, on the
// first call. Any thread.
PlatformFeatures ProbePlatformFeatures();

// --- Decisions (pure; tested without a bus)
// ------------------------------------

// True when Chromium's cookie store must be told --password-store=basic:
// the Secret Service is locked (or not running and may start locked) and no
// one here can answer its unlock prompt. Chromium would wait for that key
// forever, holding every request that carries cookies. When there is no
// Secret Service at all (or no session bus), Chromium falls back to basic
// by itself, so the choice is left to it; likewise when KWallet is present
// (Chromium's store on Plasma).
bool NeedsBasicPasswordStore(const PlatformFeatures& f);

// --- The cookie store, sticky per profile (CEF on Linux)
// -------------------------------------------------------------------

// The file in a CEF root cache directory that records the password store
// its profile chose: "basic" or "os" (left to Chromium). A profile's cookies
// are encrypted with that store's key, so a later launch must not switch:
// a headless launch would turn an "os" profile into "basic" (its cookies
// unreadable), and the reverse.
extern const char kPasswordStoreMarkerName[];

// The recorded store ("basic" / "os"), or "" when there is none or it can't
// be read (an unknown value counts as none).
std::string ReadPasswordStoreMarker(const std::string& root_cache_dir);

// Records `store` ("basic" / "os"). False when it can't be written (no
// directory: a profile kept in memory).
bool WritePasswordStoreMarker(const std::string& root_cache_dir,
                              const std::string& store);

struct PasswordStoreChoice {
  std::string store;          // "basic" or "os"
  bool append_basic = false;  // add --password-store=basic to the command line
  bool record = false;        // write `store` to the marker
  // "explicit" (--password-store on the command line), "profile" (the
  // marker), "probe" (this launch's platform features).
  std::string source;
  std::string reason;  // why basic, when the probe chose it
};

// Picks the store: an explicit --password-store wins (and is recorded, so
// the profile keeps it); else the profile's recorded choice; else the probe
// (NeedsBasicPasswordStore), recorded. `probe` runs only in the last case.
PasswordStoreChoice ChoosePasswordStore(
    const std::string* explicit_store,
    const std::string& marker,
    const std::function<PlatformFeatures()>& probe);

// Whether a tray icon can be shown, and why not ("" when it can).
bool TrayAvailable(const PlatformFeatures& f);

std::string TrayUnavailableReason(const PlatformFeatures& f);

// The secret-service situation in a sentence, for the --password-store=basic
// warning.
std::string BasicPasswordStoreReason(const PlatformFeatures& f);

// Why notifications may not show ("" when a server runs; Linux only).
std::string NotificationUnavailableReason(const PlatformFeatures& f);

// The JSON object platform_features hands out (docs/platform-features.md).
std::string PlatformFeaturesToJson(const PlatformFeatures& f);

// --- Backend hooks
// ---------------------------------------------------------------

// The cookie-store decision a CEF backend made ("os" / "basic"); reported by
// PlatformFeaturesJsonForAbi. Any thread.
void SetCookieEncryption(const char* value);

// ProbePlatformFeatures() as JSON, malloc'd for the C ABI (freed with the
// backend's string_free). Any thread.
char* PlatformFeaturesJsonForAbi();

// Test-only: forget every cached probe result (the next call probes again).
void ResetPlatformFeaturesForTesting();

// Test-only: how many times the probe connected to an X server for the
// XEmbed tray (only ever in an X11 session).
int XEmbedProbeCountForTesting();

}  // namespace laufey_common

#endif  // LAUFEY_PLATFORM_FEATURES_H_
