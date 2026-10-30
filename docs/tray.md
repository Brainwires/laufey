# Tray / status bar

A tray icon is a persistent icon in the operating system's status area: the menu
bar on macOS, the system tray on Windows, and the AppIndicator area on Linux.
Each icon has an image, a tooltip, a right-click menu, and click handlers.
`TrayIcon` is a builder; you must keep the returned value alive for the icon to
remain visible.

```rust
use laufey::{MenuItem, TrayIcon};

let tray = TrayIcon::new()
  .icon(include_bytes!("icon.png"))
  .icon_dark(include_bytes!("icon-dark.png")) // optional dark-mode variant
  .tooltip("My App")
  .menu(&[MenuItem::Role { role: "quit".into() }], |id| println!("{id}"))
  .on_click(|| println!("clicked"))
  .on_double_click(|| println!("double clicked"));

// The icon's bounds let you anchor a popover panel beneath it.
let bounds = tray.get_bounds(); // Option<(x, y, width, height)>
```

When you provide both a light and a dark icon, the backend watches the system
appearance and swaps between them live: it observes
`AppleInterfaceThemeChangedNotification` on macOS, the `WM_SETTINGCHANGE`
message together with the `AppsUseLightTheme` setting on Windows, and polls once
per event-loop tick on Winit. On Linux, from API 50, a session with a
StatusNotifierWatcher gets laufey's own StatusNotifierItem (below), which
delivers clicks; on the AppIndicator path (an XEmbed tray, or
`LAUFEY_TRAY_BACKEND=appindicator`) the icon renders through the desktop theme
and no click or double-click event arrives (`platform_features()` reports
`"trayClicks": false` there). The tooltip is the item's title, which
StatusNotifier hosts (Plasma, the GNOME AppIndicator extension) show on hover.
The CEF backend uses the same tray on Linux, so a tray icon does not require a
browser window.

## Tray events (API ≥ 50)

`laufey::set_tray_event_handler(tray_id, handler)` gets every event of the icon
with its detail: the kind (click, double click, right click, middle click, mouse
enter, mouse leave), the button, the modifiers held (`LAUFEY_MOD_*`) and the
cursor in `set_position`'s screen space. The click and double-click handlers
keep firing as before; a right click still opens the menu.

```rust
let tray = TrayIcon::new().icon(include_bytes!("icon.png"));
laufey::set_tray_event_handler(
  tray.id(),
  Some(Box::new(|e| {
    if e.kind == laufey::TrayEventKind::Click && e.modifiers & 4 != 0 {
      println!("Option/Alt-click at {:?}", e.position);
    }
  })),
);
laufey::pop_up_tray_menu(tray.id()); // open the menu now (Electron's popUpContextMenu)
```

- **macOS**: from the status item's event (left, right, other mouse up;
  `clickCount` 2 is a double click, after the first click's event), and hover
  through a tracking area on its button. Ctrl+click is a left click with the
  Control modifier: the app decides.
- **Windows**: from the notify icon's callback messages (`WM_LBUTTONUP`,
  `WM_LBUTTONDBLCLK`, `WM_RBUTTONUP`, `WM_MBUTTONUP`), modifiers from
  `GetKeyState`, the cursor in DIPs; hover starts with the first `WM_MOUSEMOVE`
  and a timer reports the leave once the cursor is off the icon's rectangle. A
  double click is click, double click, click (Windows' own message order).
- **Linux**: the StatusNotifierItem's `Activate` (click; two within the
  desktop's double-click time are a double click), `SecondaryActivate` (middle
  click) and `ContextMenu` (right click), with the point the host sends (Plasma
  does; hosts that send (0, 0) report no position). No modifiers or hover in the
  protocol; the host opens the `com.canonical.dbusmenu` menu itself, so
  `pop_up_tray_menu` is `false`.

`laufey::tray_capabilities()` (`LAUFEY_TRAY_CAP_*`) says which of these this
backend delivers here: every bit on macOS and Windows; on Linux click, double,
right, middle and position on the StatusNotifierItem path, none on AppIndicator.

### The StatusNotifierItem (Linux)

With a watcher on the session bus the WebView and CEF backends export the item
themselves over GDBus (no library needed): `org.kde.StatusNotifierItem` at
`/org/laufey/StatusNotifierItem/<id>` (registered with the watcher by object
path, as libayatana-appindicator does; registered again when a host restarts)
with `ItemIsMenu` false, the icon as `IconPixmap` (ARGB32) and as a PNG under
`$XDG_RUNTIME_DIR/laufey-tray-<pid>/` (`IconThemePath` + `IconName`), and the
menu as `com.canonical.dbusmenu` at `/org/laufey/Menu/<id>` (labels, separators,
submenus, disabled and checked items, icons). The Layer-1 observer
(`scripts/native-e2e-run.sh <backend> --layer1`) reads it like a host and calls
`Activate`, `SecondaryActivate` and `ContextMenu`.

On Linux, the CEF and WebView backends load the appindicator library at runtime
— `libayatana-appindicator3.so.1` first, falling back to the legacy
`libappindicator3.so.1`. If neither is installed, tray creation returns id `0`
and all tray calls are no-ops; the rest of the application is unaffected. If you
package an application that uses a tray icon as a `.deb`/`.rpm`, declare a
dependency on the distro's Ayatana runtime package (Debian/Ubuntu:
`libayatana-appindicator3-1`; Fedora: `libayatana-appindicator-gtk3`).

The session must also have a tray host for the icon to show: a StatusNotifier
watcher (`org.kde.StatusNotifierWatcher` on the session bus; Plasma, Cinnamon,
Ubuntu's GNOME and most others run one, stock GNOME only with the AppIndicator
extension) or, on X11, an XEmbed system tray. Without one, `create_tray_icon`
returns `0` instead of an icon no one could see, logs the reason once, and
`platform_features()` reports `"trayHost": false` with that reason in
`"trayReason"` (see [Platform features](platform-features.md)). A tray-only app
should show a window instead. The probe follows the watcher's
`NameOwnerChanged`, so a host that starts later (an extension enabled, the shell
restarted) counts at once, and the platform-features change handler
(`laufey::on_platform_features_changed`, the C ABI's
`set_platform_features_changed_handler`, API 45) fires: create the tray again
when `"trayHost"` becomes `true`. `laufey::tray_unavailable_reason()` (the C
ABI's `tray_unavailable_reason`) answers the tray part of the probe alone, so it
never waits for xdg-desktop-portal.
