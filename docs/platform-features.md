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

| Key                   | Value                                                                                                                                                                                                                                                                                                                 |
| --------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `os`                  | `"linux"`, `"macos"` or `"windows"`.                                                                                                                                                                                                                                                                                  |
| `sessionType`         | Linux: `XDG_SESSION_TYPE` as set (`"wayland"`, `"x11"`, `"tty"` for an ssh or console session), or `"unknown"` when it is unset. Never guessed from `$DISPLAY` or `$WAYLAND_DISPLAY`. `null` elsewhere.                                                                                                               |
| `desktopHint`         | `XDG_CURRENT_DESKTOP` as set, or `null`. A hint for wording only.                                                                                                                                                                                                                                                     |
| `sessionBus`          | Linux: a D-Bus session bus answered.                                                                                                                                                                                                                                                                                  |
| `trayHost`            | A tray icon can be seen: on Linux, `org.kde.StatusNotifierWatcher` has an owner, or an XEmbed tray runs on X11, and the appindicator library loads. Always `true` on macOS and Windows.                                                                                                                               |
| `trayReason`          | Why not, when `trayHost` is `false`; otherwise `null`.                                                                                                                                                                                                                                                                |
| `trayClicks`          | The icon reports clicks (`false` on Linux: AppIndicator opens its menu instead).                                                                                                                                                                                                                                      |
| `trayTooltip`         | The tooltip shows (on Linux, as the indicator's title).                                                                                                                                                                                                                                                               |
| `secretService`       | Linux: `"available"` (the default collection is unlocked), `"locked"` (locked, or missing: using it means a prompt), `"activatable"` (not running; D-Bus can start it), `"absent"`, `"no-session-bus"`. `"os"` on macOS and Windows.                                                                                  |
| `secretServicePrompt` | Someone could answer an unlock prompt: `sessionType` is `x11` or `wayland` with a display and, where logind can say, the process's logind session is an active x11 / wayland one; and, where gnome-keyring is the provider, its prompter exists. A display alone (Xvfb, cron, `xvfb-run` under systemd) never counts. |
| `portalVersions`      | Linux: `{ "Notification": 2, "FileChooser": 4, "GlobalShortcuts": 1, "Settings": 2 }` for the xdg-desktop-portal interfaces the portal offers; an interface it lacks is absent. `{}` elsewhere.                                                                                                                       |
| `cookieEncryption`    | CEF: `"basic"` when it started Chromium with `--password-store=basic` (see below), otherwise `"os"`: the store was left to Chromium, which keeps the key in the OS keystore when there is one. `null` on the WebView and Winit backends, whose engines don't encrypt cookies with an OS key.                          |

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
keyring, a locked one someone can unlock, KWallet (`org.kde.kwalletd5` /
`org.kde.kwalletd6` runs or can be started; it asks for its own unlock), and no
Secret Service at all or no session bus, where Chromium finds no keystore and
falls back to `basic` by itself without waiting. An explicit `--password-store`
on the command line is kept as given.

The choice is sticky per profile. CEF records it in the profile's root cache
directory (a `laufey-password-store` file holding `basic` or `os`), and later
launches keep it without probing again: the profile's cookies are encrypted with
that store's key, so switching would make them unreadable. A headless launch
therefore can't turn a profile created in a desktop session (`os`) into a
`basic` one and lose its cookies, nor the reverse. (Such a launch of an `os`
profile against a locked keyring no one can unlock waits for the key, as
Chromium does on its own.) Passing `--password-store` explicitly changes the
choice, and the new one is recorded. A profile with no data directory (the
throwaway per-process one) decides on every launch.
