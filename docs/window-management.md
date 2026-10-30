# Window management

Every laufey application is built around one or more native windows. A `Window`
controls its title, size, position, resizable and always-on-top flags, opacity,
click passthrough, visibility, and focus. The type is a builder, so you can
configure a window fluently when you create it, and each property also has a
plain setter you can call later while the window is open.

```rust
use laufey::Window;

let win = Window::new(800, 600)
  .title("My App")
  .position(100, 100)
  .resizable(true)
  .opacity(0.95)
  .load("index.html"); // or .navigate("https://example.com")

win.set_size(1024, 768);
let (width, height) = win.get_size();
let scale = win.get_scale_factor(); // window.devicePixelRatio
win.focus();
win.hide();
```

A few properties can only be chosen when the operating system creates the window
and cannot be changed afterwards: whether the window is frameless (drawn without
operating-system chrome), whether it is a non-activating panel that does not
steal keyboard focus, and whether it has a transparent background. You set those
through `Window::new_with_options`. Everything else is a live setter. All
positions and sizes are expressed in density-independent pixels with the origin
at the top-left of the screen. `Window::get_scale_factor` is the physical-to-DIP
ratio for that window (`window.devicePixelRatio`); it updates when the window
moves to another display. `get_inner_position` is the content-view origin
(`get_position` plus title-bar chrome), so `inner + client` is
`MouseEvent.screenX` / `screenY`. The Winit backend can create and manage
windows, but because it has no web engine it cannot navigate to a URL or execute
JavaScript.

## Opacity and transparency

These are two distinct things:

- **Opacity** (`Window::opacity` / `set_opacity` / `get_opacity`) fades the
  _entire_ window — web content and native chrome alike — by a uniform factor in
  `0.0..=1.0`, where `1.0` is fully opaque (the default), like CSS `opacity` on
  the whole window. It is a live setter you can animate at runtime. The web
  backends implement it on every desktop platform (macOS `NSWindow.alphaValue`,
  Windows layered-window alpha, Linux `gtk_widget_set_opacity`). The Winit
  backend has no opacity API, so the call is a no-op there and `get_opacity`
  returns `1.0`.

  ```rust
  win.set_opacity(0.8); // 80% opaque
  ```

- **Transparency** (`WindowOptions::transparent`) gives the window a transparent
  _background_ so the web content's own alpha composites against whatever is
  behind the window. Any region the page leaves transparent (e.g. a
  `transparent` root background) shows the desktop through it. This must be
  chosen at creation time and is commonly paired with `frameless`.

  ```rust
  use laufey::{Window, WindowOptions};

  let win = Window::new_with_options(
    400,
    300,
    WindowOptions { frameless: true, transparent: true, ..Default::default() },
  )
  .load("index.html");
  ```

  Transparency is supported by the system-WebView backend on macOS, on Linux
  (WebKitGTK, on a compositing window manager) and, from API 50, on Windows
  (WebView2: DWM blur-behind over an empty region turns on per-pixel alpha, and
  the web view's default background is transparent), and by the Winit backend.
  The CEF backend paints an opaque window in windowed mode and ignores the flag
  (it would need off-screen rendering). `overlay_capabilities()` reports
  `LAUFEY_OVERLAY_CAP_TRANSPARENT` where it takes effect (on Linux only with a
  compositor).

## Click passthrough

`Window::click_passthrough` / `set_click_passthrough` / `get_click_passthrough`
makes the window ignore _all_ mouse input — clicks, moves, and wheel events fall
through to whatever window is beneath it, like Electron's
`setIgnoreMouseEvents(true)`. Keyboard input is unaffected. It is a live setter
you can toggle at any time, intended for frameless/transparent overlay windows:
HUDs, notification toasts, screen annotations.

```rust
use laufey::{Window, WindowOptions};

let overlay = Window::new_with_options(
  400,
  300,
  WindowOptions { frameless: true, transparent: true, ..Default::default() },
)
.always_on_top(true)
.click_passthrough(true)
.load("overlay.html");

// Later, to start accepting input again:
overlay.set_click_passthrough(false);
```

Platform notes:

- **macOS** — `NSWindow.ignoresMouseEvents`; works with every backend.
- **Windows** — the top-level window gets the `WS_EX_TRANSPARENT` and
  `WS_EX_LAYERED` extended styles, which exclude it (children included) from
  mouse hit-testing. Composes with `set_opacity`, which shares the layered
  style.
- **Linux** — the window's X11/Wayland input shape region is cleared.
  Best-effort under a reparenting X11 window manager, where the WM's frame may
  still catch clicks — pair it with a frameless window (the intended overlay use
  case) for reliable behavior.
- The Winit backend uses winit's `set_cursor_hittest`, with the same platform
  behavior as above.

### Forwarding: passthrough, but still observing events

`Window::click_passthrough_forward` / `set_click_passthrough_forward` /
`get_click_passthrough_forward` keeps the window's mouse events flowing to your
registered `on_mouse_move` / `on_mouse_click` / `on_wheel` handlers _while_
passthrough is active — like Electron's
`setIgnoreMouseEvents(true, { forward: true })`. The OS still delivers every
event to the window beneath; forwarding is observation only, sourced from a
global input observer that hit-tests the overlay's frame. While passthrough is
disabled the flag has no effect, because normal per-window delivery already
fires the handlers.

You cannot selectively consume a forwarded event — hit-testing is decided by the
OS before your handler runs. The standard interactive-overlay pattern instead
toggles passthrough just-in-time: watch forwarded mouse moves and disable
passthrough when the cursor enters an interactive region, re-enable it on leave.

```rust
let overlay = Window::new_with_options(
  400,
  300,
  WindowOptions { frameless: true, transparent: true, ..Default::default() },
)
.always_on_top(true)
.click_passthrough(true)
.click_passthrough_forward(true)
.on_mouse_move(move |ev| {
  // e.g. flip set_click_passthrough(false) when (ev.x, ev.y) is over a button
})
.load("overlay.html");
```

Platform support: implemented on **macOS** (an `NSEvent` global monitor —
observing mouse events needs no extra permission) for the WebView and CEF
backends. **Windows** (a `WH_MOUSE_LL` hook), **Linux/X11** (XInput2 raw
events), and the Winit backend are not implemented yet and ignore the flag (the
getter reports `false`); **Linux/Wayland** cannot support it — the compositor
does not expose global input. One macOS caveat: global monitors never see events
delivered to your _own_ application, so if the event lands on another
(non-passthrough) window of the same app that overlaps the overlay, the
overlay's handlers do not fire for it.

## Overlay windows (API ≥ 50)

A desktop companion, a HUD or a tray popover needs more than passthrough: only
part of the window takes clicks, it stays on every Space and over full-screen
apps, it keeps off the taskbar, and on Wayland it can be placed at all.
`overlay_capabilities()` (`OverlayCapabilities`) says which of these work on
this backend and OS; a setter for something not reported changes nothing and
returns `false`.

```rust
use laufey::{InputRect, Window, WindowOptions, LAUFEY_WINDOW_LEVEL_STATUS};

let w = Window::new_with_options(
  260,
  300,
  WindowOptions {
    frameless: true,
    transparent: true,
    no_activate: true,
    overlay: true, // a layer-shell surface on wlroots / KDE Wayland
    ..Default::default()
  },
)
.load("companion.html");
// Only the character and its bubble take clicks; the rest passes through.
w.set_input_region(Some(&[InputRect { x: 40, y: 60, width: 180, height: 220 }]));
w.set_visible_on_all_workspaces(true, true);
w.set_level(LAUFEY_WINDOW_LEVEL_STATUS);
w.set_skip_taskbar(true);
```

- **Input region** (`set_input_region`, `LAUFEY_OVERLAY_CAP_INPUT_REGION`):
  rectangles in content DIPs (CSS pixels) that take mouse input; elsewhere a
  click reaches the window beneath. `None` removes the region; an empty list
  lets everything through. It is independent of `set_click_passthrough`, which
  wins while on and reads back only what it was set to. macOS has no input
  shape: the window's `ignoresMouseEvents` follows the cursor (a global and a
  local mouse monitor, no permission, plus a 30 Hz timer for a window that moves
  under a cursor at rest). Windows: `WS_EX_TRANSPARENT` follows the cursor on a
  60 Hz timer of the window's own thread, so the region is input-only (nothing
  is clipped; `SetWindowRgn` would clip the painting, and `HTTRANSPARENT` only
  passes clicks within one thread). Linux: the window's input shape (X11 SHAPE,
  Wayland `wl_surface.set_input_region`), through GDK.
- **Skip the taskbar** (`set_skip_taskbar`): Windows `ITaskbarList` `DeleteTab`
  / `AddTab`, re-applied after each show (the window stays in Alt-Tab); Linux
  X11 the skip-taskbar and skip-pager hints (`_NET_WM_STATE`); not on Wayland
  (no protocol) or macOS, where the Dock is app-level (see
  [the activation policy](dock-taskbar.md#activation-policy-api--50)).
- **Every workspace** (`set_visible_on_all_workspaces(visible, full_screen)`):
  macOS `collectionBehavior` `CanJoinAllSpaces | Stationary`, plus
  `FullScreenAuxiliary` with `full_screen` (over another app's full-screen Space
  the window must also be at `STATUS` or above, and an accessory app is the
  reliable case); Linux X11 sticky (`_NET_WM_STATE_STICKY`, `_NET_WM_DESKTOP`
  `0xFFFFFFFF`). Windows has no public API for it (a `no_activate` tool window
  shows on every virtual desktop already); a layer surface is on every workspace
  by construction.
- **Levels** (`set_level`, Electron's names): `NORMAL`, `FLOATING`, `STATUS`,
  `POP_UP_MENU`, `SCREEN_SAVER`, mapped to the macOS window levels. Windows and
  Linux have one floating level (topmost / keep-above) for every level above
  `NORMAL`.
- **Layer shell** (`WindowOptions::overlay`, `LAUFEY_OVERLAY_CAP_LAYER_SHELL`):
  on a Wayland compositor with `zwlr_layer_shell_v1` (wlroots compositors, KDE
  Plasma, COSMIC) the WebKitGTK window becomes an overlay-layer surface through
  `libgtk-layer-shell.so.0`, loaded at runtime (a package `Recommends`, not a
  build dependency): above every window, on every workspace, out of the task
  list, no keyboard focus, and placeable — `set_position` sets its margins from
  the top-left corner. GNOME has no layer shell: there, and on X11, the flag is
  ignored and the window is an ordinary one.

|                  | macOS        | Windows      | Linux X11            | Linux Wayland          |
| ---------------- | ------------ | ------------ | -------------------- | ---------------------- |
| input region     | WebView, CEF | WebView, CEF | WebView, CEF         | WebView                |
| skip taskbar     | —            | WebView, CEF | WebView, CEF         | —                      |
| every workspace  | WebView, CEF | —            | WebView, CEF         | layer shell            |
| over full screen | WebView, CEF | —            | —                    | layer shell            |
| levels           | WebView, CEF | WebView, CEF | WebView, CEF         | WebView (hint)         |
| transparent      | WebView      | WebView      | WebView (compositor) | WebView                |
| layer shell      | —            | —            | —                    | WebView (wlroots, KDE) |

The Winit backend reports none of them yet.

## Maximize, minimize and fullscreen (API ≥ 38)

```rust
use laufey::{Window, WindowState};

let win = Window::new(800, 600)
  .on_state_change(|ev| {
    // ev.state / ev.previous: WindowState { maximized, minimized, fullscreen }
    if ev.state.fullscreen && !ev.previous.fullscreen {
      println!("entered fullscreen");
    }
  })
  .load("index.html");

win.maximize();
win.unmaximize();
win.minimize();
win.restore(); // un-minimize: back to the state before minimize
win.set_fullscreen(true);
let state: WindowState = win.get_state();
```

The setters are asynchronous wherever the OS animates the change (macOS zoom,
miniaturize and the fullscreen Space transition, every Linux window manager):
`get_state()` and the state handler report the change once the OS has applied
it, never before. The handler fires once per real change, whichever OS signal
noticed it, with the state before and after; resize, move, focus and blur keep
their own handlers. A window can be minimized while it is maximized (it restores
to maximized); `restore()` only un-minimizes, `unmaximize()` only un-maximizes.

- **macOS** (WKWebView and CEF): `zoom:`, `miniaturize:` / `deminiaturize:` and
  `toggleFullScreen:` (a fullscreen Space, Electron's default). A borderless
  window has no zoom button, so "maximized" is "fills the visible frame" there.
- **Windows** (WebView2 and CEF):
  `ShowWindow(SW_MAXIMIZE / SW_MINIMIZE /
  SW_RESTORE)`; fullscreen is
  borderless and covers the whole monitor, taskbar included, and leaving it
  restores the previous style and placement.
- **Linux** (WebKitGTK and CEF): `_NET_WM_STATE` requests through GTK /
  Chromium's X11 window, applied by the window manager. Under a bare X server
  (Xvfb without a window manager) nothing applies them and the state stays
  normal; the native e2e reports those checks as N/A there and runs them under
  xfwm4 for real coverage.
- **Winit**: `set_maximized`, `set_minimized`, `Fullscreen::Borderless`.

## Size constraints (API ≥ 38)

```rust
use laufey::SizeConstraints;

win.set_min_size(400, 300);
win.set_max_size(1600, 1200);
win.set_size_constraints(SizeConstraints { min_width: 400, min_height: 300, max_width: 0, max_height: 0 });
```

Constraints are content sizes in the same units as `set_size` (see below); 0 on
an axis means no limit there, and a maximum below the minimum is raised to it.
The OS enforces them while the user resizes (`contentMinSize` / `contentMaxSize`
on macOS, `WM_GETMINMAXINFO` on Windows, GTK geometry hints, the CEF window
delegate's `GetMinimumSize` / `GetMaximumSize`, winit's min / max inner size),
`set_size` clamps to them on every backend (some OS calls, such as
`-setContentSize:` and `SetWindowPos`, ignore the limits), and a window outside
a new range is resized into it.

## Screens (API ≥ 38)

```rust
for screen in laufey::screens() {
  println!("{} {:?} work {:?} @{}x primary={}", screen.id, screen.bounds,
    screen.work_area, screen.scale_factor, screen.is_primary);
}
let mine = win.get_screen(); // Option<Screen>
laufey::on_display_changed(|| { /* re-read laufey::screens() */ });
```

`screens()` lists the connected displays, primary first. Every rectangle is in
the same space as `get_position` / `set_position` on the backend, so a saved
window position can be checked against (and clamped to) the screens directly.
`work_area` is the bounds minus the menu bar, Dock, taskbar and panels. `id`
identifies a display while it stays connected (CGDirectDisplayID on macOS, a
hash of the device name on Windows and of the panel on GTK, CEF's display id)
and fits in a JS number. `on_display_changed` fires when a display is added or
removed, moved, or changes work area or scale (macOS
`NSApplicationDidChangeScreenParametersNotification`, Windows `WM_DISPLAYCHANGE`
/ `WM_SETTINGCHANGE(SPI_SETWORKAREA)` on a hidden top-level watcher window, GDK
monitor signals; CEF on Linux has no such signal and compares the layout every 2
s instead). Winit reports a screen's work area as its bounds (winit has no
work-area API) and has no display-changed event.

## Restoring a window where the user left it (API ≥ 38)

`get_normal_bounds()` returns the bounds a window returns to when it leaves the
maximized, minimized or fullscreen state (its current bounds when it is normal),
as a position (`get_position`) and size (`get_size`). Save that, plus whether
the window was maximized, when the app quits; on the next launch check the saved
rectangle against `screens()` (a monitor may be gone), then `set_position` +
`set_size` and `maximize()` if it was. Persisting it is the app's job, laufey
keeps nothing across runs.

Windows asks the OS (`GetWindowPlacement`, converted from workspace to screen
coordinates). Elsewhere the backend records the geometry it sees while the
window is normal and keeps it once it has been unchanged for 300 ms, so the
intermediate frames of a zoom animation (which macOS reports while the window
still looks normal) never replace the frame the window had before.

## Title bar styles (API ≥ 38, macOS)

```rust
use laufey::TitlebarStyle;

win.set_titlebar_style(TitlebarStyle::HiddenInset); // false where unsupported
win.set_traffic_light_position(Some((16, 18)));      // None puts them back
```

`Hidden` is the runtime form of `WindowOptions::transparent_titlebar` (the
content extends under a transparent title bar with the system buttons overlaid);
`HiddenInset` adds the taller title bar of Electron's `hiddenInset` (an empty
unified toolbar). The traffic-light position is kept across resizes, fullscreen
and title changes. Both work on WKWebView and on CEF's NSWindow; the other
platforms have no title bar API of this kind and return `false`.

## Backdrops: Mica, Acrylic, vibrancy, blur (API ≥ 38)

```rust
use laufey::{Backdrop, VibrancyMaterial};

win.set_backdrop(Backdrop::Mica);                                 // Windows 11
win.set_backdrop(Backdrop::Vibrancy(VibrancyMaterial::Sidebar));  // macOS
win.set_backdrop(Backdrop::Blur);                                 // KDE Plasma
win.set_backdrop(Backdrop::None);
```

A backdrop shows only where the page leaves its background transparent (e.g.
`html, body { background: transparent }`); the backend makes the web view's own
background transparent while one is set.

- **Windows 11, WebView2**: `DWMWA_SYSTEMBACKDROP_TYPE` (Mica, Acrylic, tabbed
  Mica) on build 22621 and later; on 22000–22620 only Mica, through the older
  `DWMWA_MICA_EFFECT`; nothing on Windows 10. The frame is extended into the
  client area and the WebView2 default background made transparent.
- **macOS, WKWebView**: an `NSVisualEffectView` (behind-window blending) is put
  behind the web view, which stops drawing its background.
- **CEF, every OS**: not supported — the CEF browser view paints an opaque
  background in windowed mode (the same reason `WindowOptions::transparent` is
  ignored there), so nothing could show through.
- **WebKitGTK on KDE Plasma**: KWin's blur behind the window
  (`LAUFEY_BACKDROP_BLUR`; `ACRYLIC` is accepted as the same blur, so an app
  that asks for Acrylic gets the nearest thing). On Wayland through KDE's
  `org_kde_kwin_blur_manager` protocol (the whole surface, applied with the
  window's next frame and again whenever it is mapped, since GTK replaces a
  Wayland window's surface when it hides it); on X11 through the
  `_KDE_NET_WM_BLUR_BEHIND_REGION` property (an empty list, the whole window).
  Found by feature probing, never by the desktop's name: KWin offers the global,
  or lists the atom in the root window's `_NET_SUPPORTED`, only while its Blur
  desktop effect is loaded (which needs OpenGL compositing), so another
  compositor (mutter, wlroots, any X11 window manager) or KWin with the effect
  off reports no blur, and the capability is read again on each call. While a
  blur is set the window paints no background of its own and the web view's
  background is clear; `None` restores both (a window created `TRANSPARENT`
  stays transparent). On X11 the window needs an RGBA visual, chosen before it
  is realized, so only a window created with `WindowOptions::transparent` can
  show it there; any other refuses (`false`) although the session offers it. The
  window manager's own decorations are not blurred, and on Wayland a
  client-side-decorated window's shadow margin is blurred with the rest of the
  surface.
- **WebKitGTK elsewhere / Winit**: no backdrop API.

`set_backdrop` returns `false` and changes nothing when the backend / OS can't
show that backdrop.

## Capabilities (API ≥ 38)

`laufey::window_capabilities()` says what this backend can do on this OS (and
build), so an app can surface "unsupported" instead of pretending:

| Capability                | WKWebView | CEF macOS | WebView2       | CEF Windows | WebKitGTK | CEF Linux       | Winit                  |
| ------------------------- | --------- | --------- | -------------- | ----------- | --------- | --------------- | ---------------------- |
| state + state events      | ✓         | ✓         | ✓              | ✓           | ✓ (WM)    | ✓ (WM)          | ✓                      |
| size constraints          | ✓         | ✓         | ✓              | ✓           | ✓         | ✓               | ✓                      |
| screens                   | ✓         | ✓         | ✓              | ✓           | ✓         | ✓               | ✓ (work area = bounds) |
| display-changed events    | ✓         | ✓         | ✓              | ✓           | ✓         | ✓ (polled, 2 s) | –                      |
| title bar hidden / inset  | ✓         | ✓         | –              | –           | –         | –               | –                      |
| traffic-light position    | ✓         | ✓         | –              | –           | –         | –               | –                      |
| Mica / Acrylic / tabbed   | –         | –         | ✓ (Windows 11) | –           | –         | –               | –                      |
| vibrancy                  | ✓         | –         | –              | –           | –         | –               | –                      |
| blur (KWin)               | –         | –         | –              | –           | ✓ (KWin)  | –               | –                      |
| normal bounds             | ✓         | ✓         | ✓              | ✓           | ✓         | ✓               | ✓                      |
| keep-alive with no window | ✓         | ✓         | ✓              | ✓           | ✓         | ✓               | ✓                      |
| set position              | ✓         | ✓         | ✓              | ✓           | X11 only  | X11 only        | not on Wayland         |

"(WM)": applied by the window manager; nothing changes under a bare X server.
"(KWin)": while KWin's Blur effect is on, on Wayland or X11 (with Acrylic
reported too, as the same blur); `platformFeatures().windowBlur` says whether
the session's compositor offers it at all.

**Units.** Sizes, constraints, positions and screen rectangles are in DIP
(density-independent pixels: points on macOS, CSS pixels at zoom 1) on every
backend. `get_size` / `set_size` and the size constraints are the window's
**content** area, the page (`window.innerWidth` / `innerHeight`), and
`get_outer_size` is the whole window, frame included; `get_position` is the
frame's top-left corner and `get_inner_position` the content's. On Windows the
process is per-monitor DPI aware: a window's geometry is converted with the
scale of the monitor it is on, a screen's with its own, so DIP coordinates are
continuous within one monitor (as in winit) but not across monitors with
different scales. WebView2 reported physical pixels and the outer window
rectangle for `get_size` / `set_size` in earlier versions, and CEF on Windows
and macOS the whole window; both now size the content like the other backends.
The page's `window.outerWidth` / `outerHeight` and `screenX` / `screenY` are the
same frame as `get_outer_size` / `get_position`; WKWebView answered 0 for all
four before laufey answered WebKit's request for the window frame.

## The first window at launch

An app is often started by something that stays frontmost: a terminal, an IDE's
task runner, a CI agent. On macOS 14 and later an app only takes activation
cooperatively, so such a launch can leave it inactive, and a window that was
merely made key and ordered front would open behind the active app's windows.
WebKit and Chromium read a fully covered window as occluded: the page reports
`document.visibilityState` "hidden" and `requestAnimationFrame` stops until the
window is uncovered. So the first window an app shows (at creation, or with
`show()` for one created `hidden`) activates the app, becomes key and is ordered
in front of other apps' windows even when the activation is declined. A window
created `hidden` is never revealed by this, and a non-activating panel
(`no_activate`) never activates the app. Later windows are made key and ordered
front within the app, as before. A window that is covered, minimized or on a
locked screen is still throttled by the engine, as in a browser.

## Keeping a tray app alive; quitting (API ≥ 38)

By default the event loop ends when the last window closes. A tray / menu-bar
app calls `laufey::set_quit_on_last_window_closed(false)` and keeps running with
no window (it can open one again later); `laufey::quit()` ends it. On macOS an
app whose activation policy is Accessory (`set_dock_visible(false)`) also stays
alive when its last window closes, as it did before.

`laufey::quit()` ends the loop the way closing the last window does: the backend
returns from its run loop and calls the runtime's `laufey_runtime_shutdown`.
Windows still open are closed without a close-requested event (quitting is
app-level; see [window-events.md](window-events.md)). CEF closes its browsers
first and ends the loop when the last one is gone (macOS stops `[NSApp run]`,
which `CefQuitMessageLoop` did not); WebView2 posts the quit to its UI thread
(it used to post it to the calling thread's queue); Winit now calls the
runtime's shutdown once its loop has ended, like the other backends.

On Linux the CEF and WebView hosts end the same way on `SIGTERM`, `SIGINT` and
`SIGHUP` (they print `laufey: <signal>, quitting`). On CEF, Chromium's own
handlers, which `CefInitialize` installs, would end the browser process with
`_exit(0)` for `SIGTERM` (Chromium's session end), skipping the runtime's
shutdown and `CefShutdown`, so the profile was left marked `"SessionEnded"` and
the child processes found their browser gone. On WebView the signal's default
action killed the process, skipping the runtime's shutdown and the web storage
flush. A GTK dialog that is open ends as a cancel (see
[dialogs.md](dialogs.md)). The first signal starts the quit and gives the three
signals their default action back, so a second one ends a quit that hangs. The
signal is taken off the UI thread and the quit handed to it; a UI thread stuck
in a call that never returns to its loop (CEF waiting on an X server that does
not answer before its first window exists) would otherwise leave the signal
ignored until `SIGKILL`. If the UI thread has not taken the quit within
`LAUFEY_SIGNAL_QUIT_DEADLINE_SECS` (10 by default; 0 waits for ever), the
process says so on stderr and ends by the signal. A quit the UI thread has taken
is never cut short. A runtime that installs its own handlers for these signals
afterwards (Deno's `Deno.addSignalListener`) takes them over.

## External links and popups

A web-engine backend never opens a second web view for the page. A link to
another origin and a request for a new window (`window.open()`,
`<a target="_blank">`) open in the user's browser instead, under these rules:

- Only an absolute `http:` or `https:` URL with a host goes to the browser.
  Every other destination (`file:`, a custom scheme, `javascript:`,
  `about:blank`) is dropped.
- Only a request a user action started does. A page script that calls
  `window.open()` from a timer, on load or in a loop, or clicks a link with
  `element.click()`, opens nothing; the backend logs
  `laufey: not opening <url> in the browser: the page asked without a user gesture`.
  Each engine reports the gesture its own way: CEF's `OnBeforePopup`
  `user_gesture`, WebView2's `IsUserInitiated`, WebKitGTK's
  `webkit_navigation_action_is_user_gesture`. WKWebView has no public flag, so
  the macOS backend turns `javaScriptCanOpenWindowsAutomatically` off (macOS
  defaults it on) and WebKit's popup blocker drops a request without a gesture
  before it reaches the backend.
- A same-window navigation to another origin never loads in the window (the
  navigation guard, API 48, below). The page-side interceptor still hands a
  user-initiated one (the Navigation API's `userInitiated`, or on WKWebView,
  which lacks that API, a trusted click on a link) to the browser before the
  engine starts it.

### The navigation guard (API 48)

A window's top-level frame loads only:

- the origin of the URL the embedder last navigated it to (`navigate`), so the
  app's own origin, whatever it is (a custom scheme, a loopback server, a dev
  server); the embedder's own navigation is never refused;
- the origins the embedder allows for every window with
  `set_allowed_navigation_origins` (exact origins, `scheme://host[:port]`;
  `laufey::set_allowed_navigation_origins` in Rust);
- `about:blank` and `about:srcdoc`, and, in a window the embedder pointed at a
  `file:` URL, other `file:` URLs.

Any other top-level navigation (a link, `location = …`, a form, a server
redirect, a meta refresh, a script on a timer) is cancelled and logged
(`laufey: <url> is not the app's origin: opening it in the browser instead`). An
`http(s)` destination that passes the URL rule above opens in the user's
browser; anything else (`file:`, `data:`, a custom scheme, `mailto:`) is
dropped. A window hands at most 3 such navigations a second to the browser; the
rest are cancelled and logged, so a page navigating away in a loop cannot flood
the browser. Sub-frames are not checked: what a page may frame is its Content
Security Policy's (`frame-src`).

| Backend   | Where                                                        | Redirects                           |
| --------- | ------------------------------------------------------------ | ----------------------------------- |
| CEF       | `CefRequestHandler::OnBeforeBrowse` (main frame)             | asked again per redirect            |
| WKWebView | `decidePolicyForNavigationAction` (target frame is the main) | asked again per redirect            |
| WebView2  | `NavigationStarting` (main frame only)                       | raised again per redirect           |
| WebKitGTK | `decide-policy`, the main frame's main-resource response     | the final response, after redirects |

WebKitGTK's navigation policy decision does not say which frame it targets, so
that backend refuses the main frame's response instead
(`webkit_response_policy_decision_is_main_frame_main_resource`, WebKitGTK 2.40):
the engine has already sent the request, but nothing from the response is
committed or run. Before 2.40 it stops the main frame's load as it starts or
redirects.

A test can turn on `test_capture_external_opens`: every URL a backend would hand
to the browser (the guard, a popup, the reserved call) is then recorded for
`test_take_external_opens` instead of opened.

A script can also call the reserved `__laufeyOpenExternal(url)` method of the
bridge namespace directly, without a gesture. The backend applies the URL rule
to that call (so it can only ever open an `http(s)` page in the browser) but
cannot see a gesture there, since the call is an ordinary bridge message.
