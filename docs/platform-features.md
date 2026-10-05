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

| Key                   | Value                                                                                                                                                                                                                                |
| --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `os`                  | `"linux"`, `"macos"` or `"windows"`.                                                                                                                                                                                                 |
| `sessionType`         | Linux: `"wayland"`, `"x11"`, `"tty"` (an ssh or console session) or `"unknown"`, from `XDG_SESSION_TYPE`, else the display variables. `null` elsewhere.                                                                              |
| `desktopHint`         | `XDG_CURRENT_DESKTOP` as set, or `null`. A hint for wording only.                                                                                                                                                                    |
| `sessionBus`          | Linux: a D-Bus session bus answered.                                                                                                                                                                                                 |
| `trayHost`            | A tray icon can be seen: on Linux, `org.kde.StatusNotifierWatcher` has an owner, or an XEmbed tray runs on X11, and the appindicator library loads. Always `true` on macOS and Windows.                                              |
| `trayReason`          | Why not, when `trayHost` is `false`; otherwise `null`.                                                                                                                                                                               |
| `trayClicks`          | The icon reports clicks (`false` on Linux: AppIndicator opens its menu instead).                                                                                                                                                     |
| `trayTooltip`         | The tooltip shows (on Linux, as the indicator's title).                                                                                                                                                                              |
| `secretService`       | Linux: `"available"` (the default collection is unlocked), `"locked"` (locked, or missing: using it means a prompt), `"activatable"` (not running; D-Bus can start it), `"absent"`, `"no-session-bus"`. `"os"` on macOS and Windows. |
| `secretServicePrompt` | Someone could answer an unlock prompt: a graphical session (`x11` / `wayland` with a display) and, where gnome-keyring is the provider, its prompter.                                                                                |
| `portalVersions`      | Linux: `{ "Notification": 2, "FileChooser": 4, "GlobalShortcuts": 1, "Settings": 2 }` for the xdg-desktop-portal interfaces the portal offers; an interface it lacks is absent. `{}` elsewhere.                                      |
| `cookieEncryption`    | CEF: `"os"` (the cookie key is kept by the OS keystore) or `"basic"` (see below). `null` on the WebView and Winit backends, whose engines don't encrypt cookies with an OS key.                                                      |

The probe never starts the Secret Service and never asks it to unlock: it reads
the default collection's `Locked` property. The tray host is followed live
(`NameOwnerChanged` on the CEF and WebView backends, a fresh query on Winit), so
a watcher that starts after the app counts at once. The other answers are probed
once per process.

## The cookie store on Linux (CEF)

Chromium encrypts its cookie store with a key it keeps in the Secret Service.
When the service can't hand that key out without a prompt no one can answer (a
locked keyring in an ssh, CI or other headless session), or there is no service
at all, Chromium would wait for the key forever, and every request that carries
cookies (navigations, fetches, WebSocket handshakes) with it. The CEF backend
checks this before Chromium starts and, in that case, starts it with
`--password-store=basic`: cookies are stored with a fixed key (obfuscated, not
protected by the OS). It says so once on stderr and reports
`"cookieEncryption": "basic"`. An explicit `--password-store` on the command
line is kept as given.
