// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The Linux platform-features probe (platform_features_linux.cc) against
// mock services on a private D-Bus session bus (GTestDBus):
//   - no tray watcher, then a watcher that appears late (the probe follows
//     NameOwnerChanged), then one that quits;
//   - the Secret Service activatable (not running), running with its
//     default collection locked, with and without someone to answer the
//     unlock prompt, unlocked, and no session bus at all;
//   - the portal interface versions (an interface the portal lacks is
//     absent).
// Exits 77 (skipped) without dbus-daemon.

#include <gio/gio.h>
#include <glib/gstdio.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

#include "laufey_platform_features.h"

using namespace laufey_common;

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

namespace {

const char kXml[] =
    "<node>"
    " <interface name='org.freedesktop.Secret.Collection'>"
    "  <property name='Locked' type='b' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.portal.Notification'>"
    "  <property name='version' type='u' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.portal.FileChooser'>"
    "  <property name='version' type='u' access='read'/>"
    " </interface>"
    " <interface name='org.freedesktop.portal.Settings'>"
    "  <property name='version' type='u' access='read'/>"
    " </interface>"
    "</node>";

GDBusConnection* g_conn = nullptr;  // the mock services' connection
GDBusNodeInfo* g_info = nullptr;
std::atomic<bool> g_locked{true};

GVariant* GetProperty(GDBusConnection*, const gchar*, const gchar*,
                      const gchar* interface, const gchar* property, GError**,
                      gpointer) {
  if (strcmp(interface, "org.freedesktop.Secret.Collection") == 0)
    return g_variant_new_boolean(g_locked.load());
  if (strcmp(property, "version") == 0) {
    if (strcmp(interface, "org.freedesktop.portal.Notification") == 0)
      return g_variant_new_uint32(2);
    if (strcmp(interface, "org.freedesktop.portal.FileChooser") == 0)
      return g_variant_new_uint32(4);
    return g_variant_new_uint32(2);  // Settings
  }
  return nullptr;
}

const GDBusInterfaceVTable kVTable = {nullptr, GetProperty, nullptr, {}};

void Register(const char* path, const char* iface) {
  GError* error = nullptr;
  EXPECT(g_dbus_connection_register_object(
             g_conn, path, g_dbus_node_info_lookup_interface(g_info, iface),
             &kVTable, nullptr, nullptr, &error) != 0);
}

void BusCall(const char* method, const char* name) {
  GVariant* r = g_dbus_connection_call_sync(
      g_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", method,
      strcmp(method, "RequestName") == 0 ? g_variant_new("(su)", name, 0u)
                                         : g_variant_new("(s)", name),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
  EXPECT(r);
  g_variant_unref(r);
}

// Runs the default main context (where the probe's NameOwnerChanged
// subscription delivers) until `done`, up to ~5 s.
bool SpinUntil(const std::function<bool()>& done) {
  for (int i = 0; i < 500; ++i) {
    while (g_main_context_iteration(nullptr, FALSE)) {
    }
    if (done())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool TrayWatcher() {
  PlatformFeatures f;
  ProbeTray(&f);
  return f.tray_watcher;
}

void WriteService(const std::string& dir, const char* name) {
  std::string path = dir + "/" + name + ".service";
  std::string body =
      std::string("[D-BUS Service]\nName=") + name + "\nExec=/bin/false\n";
  EXPECT(g_file_set_contents(path.c_str(), body.c_str(), -1, nullptr));
}

}  // namespace

int main() {
  gchar* daemon = g_find_program_in_path("dbus-daemon");
  if (!daemon) {
    std::printf(
        "laufey_platform_features_dbus_test: dbus-daemon missing, skipped\n");
    return 77;
  }
  g_free(daemon);

  // Activatable (never startable) Secret Service and gnome-keyring, as a
  // session where gnome-keyring is installed but not running.
  gchar* services = g_dir_make_tmp("laufey-pf-XXXXXX", nullptr);
  EXPECT(services);
  WriteService(services, "org.freedesktop.secrets");
  WriteService(services, "org.gnome.keyring");

  GTestDBus* bus = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_add_service_dir(bus, services);
  g_test_dbus_up(bus);  // sets DBUS_SESSION_BUS_ADDRESS
  // A headless (ssh / CI) session: no display, no graphical session.
  setenv("XDG_SESSION_TYPE", "tty", 1);
  setenv("XDG_CURRENT_DESKTOP", "GNOME", 1);
  unsetenv("DISPLAY");
  unsetenv("WAYLAND_DISPLAY");

  GError* error = nullptr;
  g_conn = g_dbus_connection_new_for_address_sync(
      g_test_dbus_get_bus_address(bus),
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
          G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION),
      nullptr, nullptr, &error);
  EXPECT(g_conn);
  g_info = g_dbus_node_info_new_for_xml(kXml, &error);
  EXPECT(g_info);
  // The mock's method and property calls are served on its own thread.
  std::atomic<bool> ready{false};
  std::thread([&] {
    GMainContext* ctx = g_main_context_new();
    g_main_context_push_thread_default(ctx);
    // Objects registered here deliver on `ctx`.
    Register("/org/freedesktop/secrets/aliases/default",
             "org.freedesktop.Secret.Collection");
    for (const char* iface : {"org.freedesktop.portal.Notification",
                              "org.freedesktop.portal.FileChooser",
                              "org.freedesktop.portal.Settings"}) {
      Register("/org/freedesktop/portal/desktop", iface);
    }
    ready = true;
    GMainLoop* loop = g_main_loop_new(ctx, FALSE);
    g_main_loop_run(loop);
  }).detach();
  EXPECT(SpinUntil([&] { return ready.load(); }));

  // --- Nothing running ----------------------------------------------------
  PlatformFeatures f = ProbePlatformFeatures();
  EXPECT(f.os == "linux");
  EXPECT(f.session_type == "tty");
  EXPECT(f.desktop_hint == "GNOME");
  EXPECT(f.session_bus);
  EXPECT(!f.tray_watcher);
  EXPECT(!f.tray_xembed);  // no X display
  EXPECT(!TrayAvailable(f));
  EXPECT(TrayUnavailableReason(f).find("XDG_CURRENT_DESKTOP=GNOME") !=
         std::string::npos);
  EXPECT(!f.tray_clicks);
  // Installed, not running: what it would ask, no one here could answer.
  EXPECT(f.secret_service == SecretServiceState::kActivatable);
  EXPECT(!f.secret_prompter);
  EXPECT(NeedsBasicPasswordStore(f));
  // The portal isn't running (nor activatable).
  EXPECT(f.portal_versions.empty());

  // --- A tray watcher that starts late, then quits ---------------------------
  BusCall("RequestName", "org.kde.StatusNotifierWatcher");
  EXPECT(SpinUntil([] { return TrayWatcher(); }));
  BusCall("ReleaseName", "org.kde.StatusNotifierWatcher");
  EXPECT(SpinUntil([] { return !TrayWatcher(); }));

  // --- The Secret Service running, its keyring locked
  // ---------------------------
  BusCall("RequestName", "org.freedesktop.secrets");
  g_locked = true;
  ResetPlatformFeaturesForTesting();
  PlatformFeatures s;
  ProbeSecretService(&s);
  EXPECT(s.secret_service == SecretServiceState::kLocked);
  EXPECT(!s.secret_prompter);  // a tty session
  EXPECT(NeedsBasicPasswordStore(s));

  // A graphical session, but gnome-keyring's prompter is missing.
  setenv("XDG_SESSION_TYPE", "x11", 1);
  setenv("DISPLAY", ":97", 1);  // nothing there: no XEmbed tray either
  ResetPlatformFeaturesForTesting();
  s = PlatformFeatures();
  ProbeSecretService(&s);
  EXPECT(s.session_type == "x11");
  EXPECT(s.secret_service == SecretServiceState::kLocked);
  EXPECT(!s.secret_prompter);
  EXPECT(NeedsBasicPasswordStore(s));

  // ...and with it: the user unlocks the keyring when asked.
  BusCall("RequestName", "org.gnome.keyring.SystemPrompter");
  ResetPlatformFeaturesForTesting();
  s = PlatformFeatures();
  ProbeSecretService(&s);
  EXPECT(s.secret_service == SecretServiceState::kLocked);
  EXPECT(s.secret_prompter);
  EXPECT(!NeedsBasicPasswordStore(s));

  // Unlocked: no prompt at all, even headless.
  g_locked = false;
  setenv("XDG_SESSION_TYPE", "tty", 1);
  unsetenv("DISPLAY");
  ResetPlatformFeaturesForTesting();
  s = PlatformFeatures();
  ProbeSecretService(&s);
  EXPECT(s.secret_service == SecretServiceState::kAvailable);
  EXPECT(!NeedsBasicPasswordStore(s));

  // --- Portal versions
  // ---------------------------------------------------------
  BusCall("RequestName", "org.freedesktop.portal.Desktop");
  ResetPlatformFeaturesForTesting();
  f = ProbePlatformFeatures();
  EXPECT(f.portal_versions.size() == 3);
  EXPECT(f.portal_versions["Notification"] == 2);
  EXPECT(f.portal_versions["FileChooser"] == 4);
  EXPECT(f.portal_versions["Settings"] == 2);
  EXPECT(f.portal_versions.count("GlobalShortcuts") == 0);
  std::string json = PlatformFeaturesToJson(f);
  EXPECT(json.find("\"portalVersions\":{\"FileChooser\":4,"
                   "\"Notification\":2,\"Settings\":2}") != std::string::npos);

  // --- No session bus
  // ------------------------------------------------------------
  std::string address = g_test_dbus_get_bus_address(bus);
  unsetenv("DBUS_SESSION_BUS_ADDRESS");
  setenv("XDG_RUNTIME_DIR", services, 1);  // no "bus" socket in it
  ResetPlatformFeaturesForTesting();
  f = ProbePlatformFeatures();
  EXPECT(!f.session_bus);
  EXPECT(f.secret_service == SecretServiceState::kNoSessionBus);
  EXPECT(NeedsBasicPasswordStore(f));
  EXPECT(!f.tray_watcher);
  EXPECT(f.portal_versions.empty());
  setenv("DBUS_SESSION_BUS_ADDRESS", address.c_str(), 1);

  // Drop the probe's bus before stopping the daemon (g_test_dbus_down waits
  // for the session bus to go), then the service directory it watched.
  ResetPlatformFeaturesForTesting();
  g_test_dbus_down(bus);
  g_object_unref(bus);
  for (const char* name : {"org.freedesktop.secrets", "org.gnome.keyring"}) {
    std::string path = std::string(services) + "/" + name + ".service";
    g_unlink(path.c_str());
  }
  g_rmdir(services);
  g_free(services);
  std::printf("laufey_platform_features_dbus_test: OK\n");
  std::fflush(stdout);
  // The mock thread is detached and still holds its connection: end here.
  std::_Exit(0);
}
