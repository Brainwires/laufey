# Dock / taskbar

laufey can badge the application icon, request the user's attention, and — on
macOS — drive the dock menu, the icon's visibility, and a reopen callback. These
are free functions rather than window methods, because the dock is
application-scoped on macOS, while on Windows and Linux the equivalent
operations act on the currently focused window's taskbar button.

```rust
use laufey::DockBounceType;

laufey::set_dock_badge(Some("3"));      // pass None to clear the badge
laufey::bounce_dock(DockBounceType::Critical);

laufey::on_dock_reopen(|has_visible_windows| {
  // On macOS, the user clicked the dock icon while no windows were open.
});
```

On macOS the badge is a native red overlay drawn on the dock tile. On Windows it
is a `"(N) "` prefix on the window titles, the convention used by applications
such as Slack, Discord, and Telegram; taskbars and window-manager overviews
surface that title. On Linux, under the CEF and WebKitGTK backends, a badge of
digits is the count on the app's launcher where a dock reads the
`com.canonical.Unity.LauncherEntry` API: one owns `com.canonical.Unity`
(Ubuntu's dock, Dash to Dock) or `org.kde.plasmashell` runs (Plasma's task
manager), and `<app id>.desktop` is installed for the dock to match
`application://<app id>.desktop` against. Otherwise (no such dock, no installed
desktop entry, a badge that isn't a number, or the Winit backend, which sends no
launcher badge) it is the same title prefix. `platform_features` reports which
one applies (`badge`, `badgeReason`). Requesting attention bounces the dock icon
on macOS, flashes the taskbar button on Windows, and sets the window's urgency
hint on Linux. The dock menu is the Dock menu on macOS and the taskbar button's
jump list on Windows (the CEF and WebView2 backends); the ability to hide the
dock icon and the reopen callback exist only on macOS.

## The Dock menu (macOS)

`set_dock_menu` takes the same template as the application menu, so a Dock menu
can hold separators, checked items (`checked: true`, an `NSControlStateValueOn`
item, which the Dock draws with a checkmark) and submenus; icons and tooltips
are not drawn there. Choosing an item calls `on_click` with its id, with window
id 0, whether or not a window is open.

`laufey::menu_snapshot(MenuKind::Dock)` (API 59) reads the menu back as AppKit
holds it for `applicationDockMenu:` (labels, ids, enabled, checked, separators,
submenus), for self-tests; see [menus.md](menus.md#reading-a-menu-back-api-59).
`None` with no Dock menu set, and off macOS (the Windows jump list is not read
back).

## The jump list (Windows)

On Windows, under the CEF and WebView2 backends, `set_dock_menu` writes the
"Tasks" category of the jump list the taskbar button (and the Start menu entry)
shows on a right click. `menu_capabilities()` reports `dock_menu()` there (and
on macOS) and `jump_list()` only there. A jump list is not a menu, so the
template is flattened:

- a submenu's items are listed in its place, between separators (a jump list has
  no submenus);
- separators are kept, but never lead, trail or repeat;
- disabled items and roles are left out (a task can't be greyed out, and it
  starts the executable rather than acting on the running app), `checked`,
  icons, accelerators and tooltips are not shown;
- an item without an id reports its label, as in the other Windows menus; one
  whose id is longer than 512 bytes is left out.

A task is a shell link to the executable with one argument,
`--laufey-dock-item=<id>`. Choosing it starts the app again with that argument:

- With the [single-instance lock](deep-links.md) on, the launch is forwarded to
  the running app, and once the app has set its dock menu laufey calls
  `on_click` with the id (window id 0, as on macOS) instead of the
  second-instance handler, without bringing a window forward. An id that is no
  longer on the menu is dropped.
- When the app isn't running, or runs without the lock, the new process gets the
  argument on its command line; so does the second-instance handler of an app
  that hasn't set its dock menu yet in this run, since the jump list outlives
  the process. `laufey::dock_item_from_args` recognises it: exactly one
  argument, the flag followed by a non-empty id. A launch with anything more
  (such as a deep link after `--`) is not a task's.

The jump list belongs to the process's AppUserModelID, the one its taskbar
button has: an explicit one when the process set it, else the one Windows
derives from the executable's path. laufey does not set one, so a pinned
shortcut without an AppUserModelID keeps grouping with the running app.
`set_dock_menu` with a NULL template, or a menu with no task left, deletes the
jump list's tasks; otherwise they stay after the app quits. The list is written
on a worker thread; a failure is logged to stderr.

The Winit backend has no jump list (it has no single-instance lock to deliver
the clicks).

## Activation policy (API ≥ 50)

A tray / menu-bar app has no Dock icon. `set_dock_visible(false)` switches the
app to the Accessory policy at runtime, but a host that starts Regular shows the
icon (and the menu bar) for a moment first. From API 50 the macOS hosts start in
the policy the [launch configuration](launch-config.md) names
(`"activationPolicy": "accessory"`, or `LAUFEY_ACTIVATION_POLICY=accessory`), or
Accessory when the bundle's `Info.plist` has `LSUIElement` set, so an accessory
app never shows a Dock icon. Before API 50 the hosts forced Regular at launch,
which undid `LSUIElement`.

```rust
// A regular app with a Dock icon and a menu bar.
laufey::set_activation_policy(laufey::LAUFEY_ACTIVATION_POLICY_REGULAR);
// A tray app.
laufey::set_activation_policy(laufey::LAUFEY_ACTIVATION_POLICY_ACCESSORY);
assert_eq!(laufey::activation_policy(), Some(1));
```

Switching to Regular also activates the app, so the window it shows next comes
to the front. An Accessory app keeps running when its last window closes (see
[Keeping a tray app alive](window-management.md)). Windows and Linux have no
app-level policy (`activation_policy()` is `None`); keep windows off the taskbar
with `set_skip_taskbar` instead
([Overlay windows](window-management.md#overlay-windows-api--50)).

### Tray apps and the Dock's recent apps

When an app that is not kept in the Dock loses its running Dock tile, macOS adds
it to the Dock's recent-apps section (when "Show suggested and recent apps in
Dock" is on). The tile goes away when the app quits, and also when a Regular app
switches back to Accessory. So a tray app that switches to Regular while a
window is open lands in the recents for good, even after the user removes it,
and macOS offers no opt-out. An app that is Accessory from launch to quit never
gets a running tile, so it never appears there.

Keep a tray app Accessory at all times (the launch configuration's
`"activationPolicy": "accessory"`, or `LSUIElement`) and bring its windows up
with `focus()`, which activates the app ignoring other apps without changing the
policy ([Focusing a window](window-management.md#focusing-a-window)). An
Accessory app has no menu bar, so the standard menu shortcuts (Cmd+C, Cmd+V,
Cmd+W, Cmd+Q) reach the window only when the app handles them itself.
