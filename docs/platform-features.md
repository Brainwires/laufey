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

| Key                   | Value                                                                                                                                                                                                                                                                                                                                        |
| --------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `os`                  | `"linux"`, `"macos"` or `"windows"`.                                                                                                                                                                                                                                                                                                         |
| `sessionType`         | Linux: `XDG_SESSION_TYPE` as set (`"wayland"`, `"x11"`, `"tty"` for an ssh or console session), or `"unknown"` when it is unset. Never guessed from `$DISPLAY` or `$WAYLAND_DISPLAY`. `null` elsewhere.                                                                                                                                      |
| `desktopHint`         | `XDG_CURRENT_DESKTOP` as set, or `null`. A hint for wording only (no feature is guessed from it).                                                                                                                                                                                                                                            |
| `sessionBus`          | Linux: a D-Bus session bus answered.                                                                                                                                                                                                                                                                                                         |
| `trayHost`            | A tray icon can be seen: on Linux, `org.kde.StatusNotifierWatcher` has an owner, or an XEmbed tray runs on X11, and the appindicator library loads. Always `true` on macOS and Windows.                                                                                                                                                      |
| `trayReason`          | Why not, when `trayHost` is `false`; otherwise `null`. Reasons name no desktop except in a last `(XDG_CURRENT_DESKTOP=…)` part, which a runtime that may not show `desktopHint` cuts.                                                                                                                                                        |
| `trayClicks`          | The icon reports clicks (`false` on Linux: AppIndicator opens its menu instead).                                                                                                                                                                                                                                                             |
| `trayTooltip`         | The tooltip shows (on Linux, as the indicator's title).                                                                                                                                                                                                                                                                                      |
| `secretService`       | Linux: `"available"` (the default collection is unlocked), `"locked"` (locked, or missing: using it means a prompt), `"activatable"` (not running; D-Bus can start it), `"absent"`, `"no-session-bus"`. `"os"` on macOS and Windows.                                                                                                         |
| `secretServicePrompt` | Someone could answer an unlock prompt: `sessionType` is `x11` or `wayland` with a display and, where logind can say, the process's logind session is an active x11 / wayland one; and, where gnome-keyring is the provider, its prompter exists. A display alone (Xvfb, cron, `xvfb-run` under systemd) never counts.                        |
| `notificationServer`  | Linux: the name of the server that owns `org.freedesktop.Notifications` right now (its `GetServerInformation` name, `"unknown"` when it doesn't say), or `null` when nothing owns it. Read live. The Notification portal's version is no proof that notifications show: on Sway with no daemon the portal still offers it. `null` elsewhere. |
| `notificationReason`  | Why notifications may not show when `notificationServer` is `null` on Linux (no server; or one D-Bus can start, which is tried when notifications are first used: the notification permission then answers `unsupported` if it fails to start), otherwise `null`.                                                                            |
| `portalVersions`      | Linux: `{ "Notification": 2, "FileChooser": 4, "GlobalShortcuts": 1, "Settings": 2 }` for the xdg-desktop-portal interfaces the portal offers; an interface it lacks is absent. `{}` elsewhere.                                                                                                                                              |
| `cookieEncryption`    | CEF: `"basic"` when it started Chromium with `--password-store=basic` (see below), otherwise `"os"`: the store was left to Chromium, which keeps the key in the OS keystore when there is one. `null` on the WebView and Winit backends, whose engines don't encrypt cookies with an OS key.                                                 |

The probe never starts the Secret Service and never asks it to unlock: it reads
the default collection's `Locked` property. The tray host is followed live
(`NameOwnerChanged` on the CEF and WebView backends, a fresh query on Winit), so
a watcher that starts after the app counts at once. The other answers are probed
once per process.

## The cookie store on Linux (CEF)

Chromium encrypts its cookie store with a key it keeps in the Secret Service
(or, on Plasma, in KWallet). When the Secret Service is locked, or not running
and may start locked, and no one can answer its unlock prompt (an ssh, CI or
other headless session: see `secretServicePrompt`), Chromium waits for the key
forever, and every request that carries cookies (navigations, fetches, WebSocket
handshakes) waits with it. The CEF backend checks this before Chromium starts
and, in that case, starts it with `--password-store=basic`: cookies are stored
with a fixed key (obfuscated, not protected by the OS). It says so once on
stderr and reports `"cookieEncryption": "basic"`.

Everything else is left to Chromium (`"cookieEncryption": "os"`): an unlocked
keyring, a locked one someone can unlock, KWallet where Chromium would use it (a
KDE desktop by Chromium's own rule: the first desktop it knows in
`XDG_CURRENT_DESKTOP` is `KDE`, else `DESKTOP_SESSION` / `KDE_FULL_SESSION`; or
`org.kde.kwalletd5` / `org.kde.kwalletd6` running now; it asks for its own
unlock), and no Secret Service at all or no session bus, where Chromium finds no
keystore and falls back to `basic` by itself without waiting. An explicit
`--password-store` on the command line is kept as given.

The OS key is sticky per profile. Once a profile gets it, CEF records `os` in
the profile's root cache directory (a `laufey-password-store` file, written to a
temporary file and renamed over it), and later launches keep asking for it.
`basic` is never recorded: Chromium still reads cookies written under `basic`
when it has the OS key, so a profile created in a headless session moves to the
OS key without losing anything the first time a launch can reach it. When an
`os` profile meets a Secret Service that is locked (or not running) with no one
to answer its prompt, that launch alone uses `basic`
(`"cookieEncryption":
"basic"`), the marker stays `os`, and stderr says once
that the cookies stored with the OS key are unavailable this run; the next
launch that can reach the key reads them again. An explicit `--password-store`
is kept as given; an OS store given that way is recorded, `basic` is not. A
profile with no data directory (the throwaway per-process one) decides on every
launch.
