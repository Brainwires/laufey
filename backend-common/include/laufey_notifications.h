// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Notifications (API 41): the portable core every CEF and WebView backend
// shares, over one per-OS NotificationPlatform:
//
//   notifications_mac.mm    UNUserNotificationCenter (calendar /
//                           time-interval triggers, categories, the delegate
//                           installed at launch for cold-start clicks)
//   notifications_win.cc    toast notifications (ScheduledToastNotification,
//                           the per-user AppUserModelID registration and its
//                           COM activator for clicks while not running)
//   notifications_linux.cc  org.freedesktop.Notifications over D-Bus, with
//                           laufey's own scheduler (persisted and re-armed at
//                           launch)
//
// The core keeps the notifications this process shows (their callbacks, by
// tag), routes the OS's events to them, sends clicks no live callback owns to
// the response handler (buffering them until one is registered: the
// cold-start click), validates options and builds the JSON the C ABI hands
// out. See docs/notifications.md.

#ifndef LAUFEY_NOTIFICATIONS_H_
#define LAUFEY_NOTIFICATIONS_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "laufey.h"
#include "laufey_backend_common.h"

namespace laufey_common {

// A notification the OS holds for later delivery (or the store re-arms).
struct ScheduledNotification {
  std::string tag;
  std::string title;
  std::string body;
  int64_t at_ms = 0;  // Unix time
  bool has_data = false;
  std::string data;
  std::vector<NotificationAction> actions;
  bool silent = false;
  bool require_interaction = false;
  std::vector<uint8_t> icon_png;
};

// The OS side, one per process (InstallNotificationPlatform). Every method
// may be called from any thread.
class NotificationPlatform {
 public:
  virtual ~NotificationPlatform() = default;
  // LAUFEY_NOTIFICATION_CAP_* bits.
  virtual uint32_t Capabilities() = 0;
  // Show `opts` now, or schedule it for opts.schedule_at_ms. `opts.tag` is
  // set (the caller's, or a generated one). Returns false when it failed at
  // once; a later failure is reported with DispatchNotificationClosed.
  virtual bool Show(const NotificationOptions& opts) = 0;
  // Remove the delivered notification and the pending request with `tag`.
  virtual void Remove(const std::string& tag) = 0;
  // The pending scheduled notifications this app made, any order.
  virtual void ListScheduled(
      std::function<void(std::vector<ScheduledNotification>)> done) = 0;
  // LAUFEY_PERMISSION_STATUS_* for LAUFEY_PERMISSION_NOTIFICATIONS(_
  // PROVISIONAL); `done` fires exactly once, on any thread.
  virtual void QueryPermission(int kind, std::function<void(int)> done) = 0;
  virtual void RequestPermission(int kind, std::function<void(int)> done) = 0;
};

// Installs the platform (later calls replace it; a replaced platform is
// leaked on purpose, as its threads may still run). Without one every
// notification fails and capabilities are 0.
void InstallNotificationPlatform(std::unique_ptr<NotificationPlatform> p);

// The installed platform, created with CreateNotificationPlatform on first
// use.
NotificationPlatform* EnsureNotificationPlatform();

// The per-OS platform, created on first use by EnsureNotificationPlatform.
std::unique_ptr<NotificationPlatform> CreateNotificationPlatform();

// Starts what must run at launch, before the runtime loads: the macOS
// notification-center delegate (a click that launched the app is delivered
// to it as AppKit finishes launching), the Windows COM activator (registered
// so a click on a toast while the app isn't running can launch it and be
// delivered), the Linux scheduler (re-arming the persisted schedule). Call
// once from each backend's startup; installs the platform. Idempotent.
void InitNotificationsAtLaunch();

// --- The laufey_backend_api_t entry points (see laufey.h) ---

uint32_t ShowNotification(const NotificationOptions& opts,
                          laufey_notification_event_fn on_event,
                          void* user_data);
void CloseNotification(uint32_t notification_id);
uint32_t NotificationCapabilities();
void SetNotificationResponseHandler(laufey_notification_response_fn handler,
                                    void* user_data);
void ListScheduledNotifications(laufey_notification_list_fn callback,
                                void* user_data);
void CancelNotification(const char* tag);
bool TestNotificationRespond(const char* tag, const char* action_id);
void QueryNotificationPermission(int kind, laufey_permission_callback_fn cb,
                                 void* user_data);
void RequestNotificationPermission(int kind, laufey_permission_callback_fn cb,
                                   void* user_data);

// --- From the platform ---

// The OS showed the notification `tag`.
void DispatchNotificationShown(const std::string& tag);
// The user clicked the notification `tag`: its body (`action` null) or an
// action button. `data` is what the OS kept with it (null if unknown; the
// core falls back to what this process recorded). Goes to the live
// notification's callback, else to the response handler (or its buffer).
// Returns true if a callback or handler received it. `launch` marks the
// response as the click that started this process (Windows' cold start) even
// when a handler was already registered when it arrived.
bool DispatchNotificationClick(const std::string& tag, const char* action,
                               const std::string* data, bool launch = false);
// The notification `tag` was dismissed, expired or failed to show.
void DispatchNotificationClosed(const std::string& tag);

// --- Portable helpers (tested) ---

// Checks the API 41 rules: a non-empty title, the tag and data byte limits,
// a tag for "schedule_at", action ids unique. False with `error` set.
bool ValidateNotificationOptions(const NotificationOptions& opts,
                                 std::string* error);

// The tag a notification without one gets: unique to this process and run.
std::string GenerateNotificationTag(uint32_t notification_id);

// Unix time now, in milliseconds.
int64_t UnixTimeMs();

// JSON string literal for `s` (quotes included), escaping per RFC 8259.
std::string JsonQuote(const std::string& s);

// The response object (see laufey_notification_response_fn).
std::string BuildNotificationResponseJson(const std::string& tag,
                                          const char* action,
                                          const std::string* data, bool launch);

// The list_scheduled_notifications array, soonest first.
std::string BuildScheduledListJson(std::vector<ScheduledNotification> list);

// The Linux schedule file (also used by tests): a JSON object
// {"version":1,"notifications":[...]} with the fields of
// ScheduledNotification (the icon base64-encoded).
std::string SerializeSchedule(const std::vector<ScheduledNotification>& list);
bool ParseSchedule(const std::string& text,
                   std::vector<ScheduledNotification>* out, std::string* error);

// The key=value argument string a Windows toast carries back on activation
// (and its parser): tag, action and data, percent-encoded.
std::string EncodeToastArguments(const std::string& tag, const char* action,
                                 const std::string* data);
bool DecodeToastArguments(const std::string& args, std::string* tag,
                          std::string* action, bool* has_action,
                          std::string* data, bool* has_data);

// Base64 (RFC 4648, padded).
std::string Base64Encode(const std::vector<uint8_t>& bytes);
bool Base64Decode(const std::string& text, std::vector<uint8_t>* out);

// --- Linux: activation and scheduled launches (portable helpers, tested) ---
//
// A click on a notification posted through the xdg-desktop-portal reaches
// the app as org.freedesktop.Application.ActivateAction on its D-Bus name
// (the app id). When the app isn't running, D-Bus starts it from the
// service file a .deb / .rpm installs (<app id>.service), whose Exec line
// carries kDBusActivationArg: the click that arrives next is the launch.
// A notification scheduled for while the app is closed is a transient
// systemd user timer that runs `<exe> kNotifyLaunchArg <id>`: the host
// posts the stored notification and exits, before any web engine or the
// runtime loads (RunNotifyLaunch). See docs/notifications.md.

extern const char kDBusActivationArg[];  // "--laufey-dbus-activated"
extern const char kNotifyLaunchArg[];    // "--laufey-notify"

// The GApplication action every notification click is sent as ("app." +
// this on the portal), its target the EncodeToastArguments string.
extern const char kNotificationActionName[];  // "laufey-notification"

// Whether `args` (argv without the program) are a scheduled-notification
// launch: exactly kNotifyLaunchArg and a NotificationTagId. Sets `id`.
bool ParseNotifyLaunch(const std::vector<std::string>& args, std::string* id);

// Whether `args` (argv without the program) carry kDBusActivationArg before
// any "--".
bool HasDBusActivationArg(const std::vector<std::string>& args);

// Fills `out` with a NULL-terminated copy of argv (the same string
// pointers) without kDBusActivationArg (before any "--"), and returns
// whether it was there. argv itself is never changed: the C runtime passes
// the process's original argc and argv to the .init_array functions of
// every library loaded later (the runtime's), so an entry moved out of it
// in place reads as NULL there (Deno's initializer called strlen on it and
// the app crashed while its runtime loaded). The host's main() calls it
// first, passes the answer to SetDBusActivationLaunch and hands `*out`
// (out->size() - 1 arguments) to everything after it: the web engine, GTK,
// the single-instance forwarding. A runtime reads the process's own
// arguments, so it leaves kDBusActivationArg out itself (laufey::args_os).
bool CopyArgvWithoutDBusActivationArg(int argc,
                                      char* const* argv,
                                      std::vector<char*>* out);

// Whether `app_id` can be the app's D-Bus name and GApplication id: at most
// 255 bytes, at least two '.'-separated elements of [A-Za-z0-9_-], none
// empty or starting with a digit.
bool IsValidApplicationId(const std::string& app_id);

// "/" + app_id with '.' as '/' and '-' as '_': where the shell and the
// portals call org.freedesktop.Application (GApplication's rule).
std::string ApplicationObjectPath(const std::string& app_id);

// 16 lowercase hex digits (FNV-1a 64) of a notification tag: the id a
// scheduled launch and its timer unit name carry instead of the tag itself.
std::string NotificationTagId(const std::string& tag);

// The app id as a timer unit name carries it: characters a unit name can't
// hold become '_', and an id longer than kTimerUnitAppIdMax keeps its first
// kTimerUnitAppIdMax - 9 characters plus '_' and 8 hex digits of its own
// NotificationTagId, so "laufey-<this>-<tag id>.service" stays within
// systemd's 255 characters. Deterministic: a package's removal script
// computes the same.
constexpr size_t kTimerUnitAppIdMax = 200;
std::string NotificationTimerAppPart(const std::string& app_id);

// "laufey-<NotificationTimerAppPart>-<NotificationTagId>": the systemd unit
// name (without ".timer" / ".service") of the tag's timer.
std::string NotificationTimerUnit(const std::string& app_id,
                                  const std::string& tag);

// The systemd glob that matches every one of the app's notification timers
// and no other app's: "laufey-<app part>-" then exactly 16 "[0-9a-f]" and
// ".timer" (an app whose id extends this one's, "<id>-extra", has more
// before its tag id). What a package's removal stops.
std::string NotificationTimerGlob(const std::string& app_id);

// The OnCalendar= value for the first whole second at or after `at_ms` (Unix
// time, ms), in UTC: "2026-10-06 14:03:07 UTC".
std::string SystemdCalendarUtc(int64_t at_ms);

// `text` with '&', '<' and '>' escaped, for a notification server that
// reads the body as markup ("body-markup").
std::string EscapeNotificationMarkup(const std::string& text);

// What the Linux notification platform can do in this session, with the
// reason for each "no" (platform_features). Empty transport: no server.
struct NotificationFacts {
  std::string transport;  // "portal", "freedesktop", or "" (no server)
  bool cold_start = false;
  std::string cold_start_reason;
  bool schedule_while_closed = false;
  std::string schedule_reason;
  bool server_caps_known = false;
  std::vector<std::string> server_caps;  // the server's GetCapabilities
};

// Linux: the facts above, read from the notification platform (created on
// first use). Elsewhere: empty.
NotificationFacts LinuxNotificationFacts();

// Linux: runs a scheduled-notification launch (ParseNotifyLaunch): when the
// app isn't running, posts the stored notification `id` (claimed from the
// schedule file, so it is posted once) and returns; the host then exits
// with the result. Never loads a web engine or the runtime. 0 elsewhere.
int RunNotifyLaunch(const std::string& id);

// Linux: this process was started by D-Bus activation (its arguments
// carry kDBusActivationArg; see CopyArgvWithoutDBusActivationArg). The
// host's main() calls it before InitNotificationsAtLaunch.
void SetDBusActivationLaunch(bool activated);

// Test-only: treat this process as started by D-Bus activation (as if its
// arguments carried kDBusActivationArg) before InitNotificationsAtLaunch.
void SetDBusActivationLaunchForTesting(bool activated);

// Test-only: forget every live notification, buffered response and the
// handler (between test cases).
void ResetNotificationsForTest();

}  // namespace laufey_common

#endif  // LAUFEY_NOTIFICATIONS_H_
