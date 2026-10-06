# Platform features

Desktops differ, Linux ones most: one session has a tray host and another
doesn't, the Secret Service may be locked with no one to unlock it, and
xdg-desktop-portal offers different interfaces at different versions.
`platform_features` (API 45) reports what **this** session provides, probed from
the session itself, never guessed from the desktop's name. `XDG_CURRENT_DESKTOP`
is read only to word a reason. A feature the session lacks is reported, with a
reason, instead of failing silently.

```rust
if let Some(json) = laufey::platform_features() {
  println!("{json}");
}
```

The C ABI entry point returns the same JSON object as a string the caller frees
with `string_free`. It may be called from any thread. On Linux the first call
may wait a few seconds for xdg-desktop-portal to start.

## Keys

| Key                               | Value                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                |
| --------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `os`                              | `"linux"`, `"macos"` or `"windows"`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| `sessionType`                     | Linux: `XDG_SESSION_TYPE` as set (`"wayland"`, `"x11"`, `"tty"` for an ssh or console session), or `"unknown"` when it is unset. Never guessed from `$DISPLAY` or `$WAYLAND_DISPLAY`. `null` elsewhere.                                                                                                                                                                                                                                                                                                                              |
| `desktopHint`                     | `XDG_CURRENT_DESKTOP` as set, or `null`. A hint for wording only (no feature is guessed from it).                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| `sessionBus`                      | Linux: a D-Bus session bus answered.                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| `trayHost`                        | A tray icon can be seen: on Linux, `org.kde.StatusNotifierWatcher` has an owner, or an XEmbed tray runs on X11, and the appindicator library loads. Always `true` on macOS and Windows.                                                                                                                                                                                                                                                                                                                                              |
| `trayReason`                      | Why not, when `trayHost` is `false`; otherwise `null`. Reasons name no desktop except in a last `(XDG_CURRENT_DESKTOP=…)` part, which a runtime that may not show `desktopHint` cuts.                                                                                                                                                                                                                                                                                                                                                |
| `trayClicks`                      | The icon reports clicks (`false` on Linux: AppIndicator opens its menu instead).                                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| `trayTooltip`                     | The tooltip shows (on Linux, as the indicator's title).                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| `secretService`                   | Linux: `"available"` (the default collection is unlocked), `"locked"` (locked, or missing: using it means a prompt), `"activatable"` (not running; D-Bus can start it), `"absent"`, `"no-session-bus"`. `"os"` on macOS and Windows.                                                                                                                                                                                                                                                                                                 |
| `secretServicePrompt`             | Someone could answer an unlock prompt: `sessionType` is `x11` or `wayland` with a display and, where logind can say, the process's logind session is an active x11 / wayland one; and, where gnome-keyring is the provider, its prompter exists. A display alone (Xvfb, cron, `xvfb-run` under systemd) never counts.                                                                                                                                                                                                                |
| `kwallet`                         | Linux, where Chromium's cookie store would use KWallet (a KDE desktop by Chromium's own rule, see below): `"open"` (kwalletd runs and its local wallet is open), `"closed"`, `"disabled"` or `"not-running"`, read from kwalletd without starting it. `null` elsewhere, and on Winit.                                                                                                                                                                                                                                                |
| `notificationServer`              | Linux: the name of the server that owns `org.freedesktop.Notifications` right now (its `GetServerInformation` name, `"unknown"` when it doesn't say), or `null` when nothing owns it. Read live. The Notification portal's version is no proof that notifications show: on Sway with no daemon the portal still offers it. `null` elsewhere.                                                                                                                                                                                         |
| `notificationReason`              | Why notifications may not show when `notificationServer` is `null` on Linux (no server; or one D-Bus can start, which is tried when notifications are first used: the notification permission then answers `unsupported` if it fails to start), otherwise `null`.                                                                                                                                                                                                                                                                    |
| `portalVersions`                  | Linux: `{ "Notification": 2, "FileChooser": 4, "GlobalShortcuts": 1, "Settings": 2 }` for the xdg-desktop-portal interfaces the portal offers; an interface it lacks is absent. `{}` elsewhere.                                                                                                                                                                                                                                                                                                                                      |
| `cookieEncryption`                | CEF: `"basic"` when it started Chromium with `--password-store=basic` (see below), otherwise `"os"`: the OS keystore (Chromium falls back to basic by itself when there is none). `null` on the WebView and Winit backends, whose engines don't encrypt cookies with an OS key.                                                                                                                                                                                                                                                      |
| `cookieEncryptionWait`            | CEF on Linux: why this launch keeps the OS key although no one may be able to unlock it (the profile holds cookies encrypted with the OS key, which `basic` would delete; see below). Requests that carry cookies wait until the keystore is unlocked. `null` otherwise.                                                                                                                                                                                                                                                             |
| `notificationTransport`           | Linux: how notifications are sent, `"portal"` (xdg-desktop-portal's Notification interface, with this app's id registered and its desktop entry installed) or `"freedesktop"` (`org.freedesktop.Notifications` itself); `null` with no notification server, and elsewhere. See [notifications.md](notifications.md#linux).                                                                                                                                                                                                           |
| `notificationColdStart`           | Linux: a click on a notification starts the app when it isn't running (the portal transport, a server with actions, `<app id>.service` installed, and this process owning the app's D-Bus name). `null` elsewhere (macOS and Windows always can).                                                                                                                                                                                                                                                                                    |
| `notificationColdStartReason`     | Why not, when `notificationColdStart` is `false`; otherwise `null`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                  |
| `notificationScheduleWhileClosed` | Linux: a scheduled notification is posted while the app is closed (a systemd user manager answers on the session bus: each one gets a transient timer). `null` elsewhere (macOS and Windows always do).                                                                                                                                                                                                                                                                                                                              |
| `notificationScheduleReason`      | Why not, when `notificationScheduleWhileClosed` is `false`; otherwise `null`.                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| `notificationServerCapabilities`  | Linux: the running notification server's `GetCapabilities` (`["actions", "body", "body-markup", "persistence", …]`); without `"actions"` no button or click is offered. `null` with no server running, and elsewhere.                                                                                                                                                                                                                                                                                                                |
| `badge`                           | How the dock badge shows: `"dock"` (macOS), `"launcher-entry"` (Linux: the count on the app's launcher, where a dock reads `com.canonical.Unity.LauncherEntry`: one owns `com.canonical.Unity`, as Ubuntu's dock and Dash to Dock do, or `org.kde.plasmashell` runs, for Plasma's task manager; and `<app id>.desktop` is installed), `"title"` (a `"(N) "` prefix on the window titles: Windows, Linux otherwise, and the Winit backend everywhere but macOS, which sends no launcher badge; also for a badge that isn't a number). |
| `badgeReason`                     | Linux, when `badge` is `"title"`: why no launcher shows it; otherwise `null`.                                                                                                                                                                                                                                                                                                                                                                                                                                                        |
| `fileChooser`                     | Linux: the chooser a file dialog uses (API 47): `"portal"` (xdg-desktop-portal's FileChooser, the desktop's own dialog) or `"gtk"` (GTK's chooser: no FileChooser in the portal, or `LAUFEY_FILE_CHOOSER=gtk`). See [file-dialogs.md](file-dialogs.md#linux-which-chooser). `null` elsewhere.                                                                                                                                                                                                                                        |
| `fileChooserReason`               | Why GTK's, when `fileChooser` is `"gtk"`; otherwise `null`.                                                                                                                                                                                                                                                                                                                                                                                                                                                                          |

The probe never starts the Secret Service and never asks it to unlock: it reads
the default collection's `Locked` property. The tray host is followed live
(`NameOwnerChanged` on the CEF and WebView backends, a fresh query on Winit), so
a watcher that starts after the app counts at once. The other answers are probed
once per process.

## The cookie store on Linux (CEF)

Chromium encrypts its cookie store with a key it keeps in the Secret Service or,
on KDE, in KWallet. When no one here can hand that key out, Chromium waits for
it forever, and every request that carries cookies (navigations, fetches,
WebSocket handshakes) waits with it:

- the Secret Service is locked, or not running and may start locked, and no one
  can answer its unlock prompt (an ssh, CI or other headless session: see
  `secretServicePrompt`);
- Chromium would use KWallet (a KDE desktop by Chromium's own rule: the first
  desktop it knows in `XDG_CURRENT_DESKTOP` is `KDE`; else `DESKTOP_SESSION` is
  `kde4`, `kde-plasma` or `kde`; else `KDE_FULL_SESSION` is set and
  `GNOME_DESKTOP_SESSION_ID` is not, a variable counting as set even when
  empty), and its wallet isn't open (`kwallet` is `"closed"`, `"disabled"` or
  `"not-running"`). On Plasma, Chromium's request for a closed wallet's key is
  never answered, with a person in front or not, so a prompter doesn't count
  there. On any other desktop Chromium uses the Secret Service, even with
  kwalletd running.

The CEF backend checks this before Chromium starts. What it does then depends on
the profile's own cookie database (`<root cache>/Default/Cookies`, read
read-only with the system SQLite before Chromium opens it):

- **No cookies encrypted with the OS key** (no database, or no row whose
  `encrypted_value` starts with `v11`): it starts Chromium with
  `--password-store=basic`. Cookies are then stored with a fixed key
  (obfuscated, not protected by the OS). It says so once on stderr and reports
  `"cookieEncryption": "basic"`. Nothing is lost: Chromium still reads those
  (`v10`) cookies once it has the OS key, so the profile moves to the OS key the
  first time a launch can reach it.
- **Cookies encrypted with the OS key** (`v11` rows), or a database that can't
  be read (corrupt, held by a running instance, no `libsqlite3`): it never
  switches to `basic`. Chromium drops a cookie it can't decrypt and then deletes
  every cookie of that site from the database, so one `basic` launch would lose
  them for good. The OS store is kept: stderr says once that the cookie store
  waits until the keystore is unlocked, and why; `"cookieEncryption"` is `"os"`
  and `"cookieEncryptionWait"` carries the reason. Requests that carry cookies
  wait until someone unlocks the keyring or opens the wallet.

Otherwise the store is left to Chromium (`"cookieEncryption": "os"`): an
unlocked keyring, a locked one someone can unlock, an open KWallet, and no
Secret Service at all or no session bus, where Chromium finds no keystore and
falls back to `basic` by itself without waiting.

An explicit `--password-store` on the command line is kept as given. An explicit
`--password-store=basic` on a profile with cookies encrypted with the OS key is
honoured too, with a warning on stderr that Chromium will delete them.

Once a profile gets the OS key, CEF records `os` in its root cache directory (a
`laufey-password-store` file, written to a synced temporary file renamed over
it; temporary files a crashed launch left are removed). It is a hint only: the
cookie database decides, and `basic` is never recorded. A profile with no data
directory (the throwaway per-process one) has no cookies to lose.
