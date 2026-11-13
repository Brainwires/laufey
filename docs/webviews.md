# Child web views

A child web view (API 58) is a second, native browser view inside a window, laid
over the window's page at bounds the embedder sets. It loads http(s) sites,
including ones that refuse to be framed, so an app can show a web page next to
its own UI without an `<iframe>`.

```rust
use laufey::{create_webview, WebViewBounds, WebViewOptions};

let view = create_webview(
  window.id(),
  &WebViewOptions {
    url: "https://github.com/".into(),
    bounds: WebViewBounds { x: 0.0, y: 48.0, width: 800.0, height: 552.0 },
    partition: Some("github".into()),
    ..Default::default()
  },
  |event| println!("{} {}", event.kind.name(), event.detail_json),
)?;
view.set_bounds(WebViewBounds { x: 0.0, y: 48.0, width: 1024.0, height: 700.0 });
view.navigate("https://github.com/denoland")?;
```

## Engines

| Backend                     | Engine                                                                                  | Capabilities                                                                                                                                                                                |
| --------------------------- | --------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| CEF                         | An Alloy-style `CefBrowserView` added with `CefWindow::AddOverlayView` (custom docking) | everything; persistent partitions when the app's profile is on disk                                                                                                                         |
| WebView, macOS              | A second `WKWebView`, a subview of the window's page view                               | everything; persistent partitions on macOS 14+ (`WKWebsiteDataStore` per partition), permission prompts on macOS 12+                                                                        |
| WebView, Windows (WebView2) | A second `ICoreWebView2Controller` in a host window above the page's controller         | everything but zoom, find, DevTools, context menu, print, clipboard and unhandled keys (below), on a runtime with profiles (`ICoreWebView2Environment10`; else `webview_capabilities` is 0) |
| WebView, Linux (WebKitGTK)  | A second `WebKitWebView`, a child of the `GtkOverlay` that holds the window's page view | everything but zoom, find, DevTools, context menu, print and clipboard (below); persistent partitions when the app has a data directory, evaluate on WebKitGTK 2.40+                        |
| Winit                       | None (the entry points are NULL)                                                        |                                                                                                                                                                                             |

`webview_capabilities` reports `LAUFEY_WEBVIEW_CAP_*` and the engine (bits
24–27). On a backend without web views `create_webview` answers
`LAUFEY_WEBVIEW_UNSUPPORTED`.

## Isolation

A web view is a browsing context of its own:

- **No bridge.** Its pages get no injected bridge or external-link script, no
  JS-call handler hears them, and no binding or custom-scheme handler is
  reachable from them. On CEF it has its own `CefClient`, which drops every
  process message its renderer sends, and the renderer is told (through the
  browser's `extra_info`) never to install the bridge. On WebView2 its web view
  has no init script, web messages and host objects off, and no
  `WebResourceRequested` filter (the environment's custom schemes are served
  only for the page's controllers).
- **Its own storage.** `partition` names a storage profile (cookies,
  localStorage, IndexedDB, cache) shared by the views with the same name and
  never the app's own: a `CefRequestContext` per name under
  `<profile>/WebViews/<name>` on CEF, a `WKWebsiteDataStore` for a UUID derived
  from the name on macOS 14+ (`PartitionUuid`), a WebView2 profile in the app's
  user data folder (`wv~<name with ':' as '~'>`, or `wvh~<hash>` for a name with
  upper case, over 61 characters or a final `.`, as profile names ignore case
  and stop at 64) on Windows, InPrivate for an ephemeral one, a
  `WebKitWebContext` over a `WebKitWebsiteDataManager` under
  `<app data dir>/WebViews/<name>` on WebKitGTK. `"ephemeral"` or
  `"ephemeral:<name>"` is kept in memory; so is every partition where
  `LAUFEY_WEBVIEW_CAP_PERSISTENT_PARTITIONS` is not reported (macOS before 14, a
  CEF app with an in-memory profile, a WebKitGTK app without a data directory:
  no `LAUFEY_DATA_DIR` / `LAUFEY_APP_ID`). The default is `"webview:default"`.
- **None of the window's navigation rules.** The window's navigation guard and
  will-navigate handler never see the view; its main frame may load http(s)
  pages and `about:blank`, and any other scheme (`file:`, `data:`,
  `javascript:`, the app's custom schemes, `mailto:`) is cancelled.

## Popups, downloads and permissions

| Option        | Values                                                                                                                                                                     |
| ------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `allowPopups` | `"same-view"` (default): the popup's URL loads in the view. `"browser"`: it opens in the OS browser. `"deny"`: nothing opens and a `NEW_WINDOW` event says what was asked. |
| `downloads`   | `"deny"` (default), `"ask"` (a save panel; WebView2's own download UI), `"browser"` (the URL opens in the OS browser).                                                     |
| `permissions` | `"deny"` (default: refused without a prompt), `"ask"` (the engine's prompt, or a message dialog naming the origin on CEF and WebKitGTK).                                   |

No second view is ever created for a popup: the page's `window.open()` returns
`null`.

## Events

Every event carries a JSON object; `CLOSED` is the last one and fires exactly
once, after `webview_close` or when the window closes. See the event kinds in
`laufey.h`. `CONSOLE` events are capped at 200 per second per view; on WKWebView
and WebKitGTK they come from a page-world script (WebKit reports no console
messages to the embedder), which the page could also post to. On WebKitGTK that
script also tells a refused popup's `NEW_WINDOW` disposition (`"popup"` when
`window.open()` was given a size or position), since WebKitGTK reports window
features only to a view it would create.

## Evaluate and capture

`webview_evaluate` runs a classic script in an isolated world of the main frame
(it shares the DOM, not the page's globals), awaits a promise result and returns
JSON. CEF and WebView2 use DevTools (`Page.createIsolatedWorld`,
`Runtime.evaluate`; `CallDevToolsProtocolMethod` on WebView2), which no page
Content-Security-Policy restricts. WKWebView runs it in a `WKContentWorld`
through an indirect `eval`, which a page CSP without `'unsafe-eval'` may refuse
(the call then fails with `SCRIPT_ERROR`). WebKitGTK does the same in a script
world named `laufey-webview` (`webkit_web_view_call_async_javascript_function`,
WebKitGTK 2.40+; older versions report no `LAUFEY_WEBVIEW_CAP_EVALUATE`).

`webview_capture` encodes what the view shows as PNG or JPEG, in device pixels
(CEF and WebView2: `Page.captureScreenshot`; WKWebView:
`takeSnapshotWithConfiguration`; WebKitGTK: `webkit_web_view_get_snapshot` of
the visible region).

## Stacking

A web view is native and draws above the window's page; among the views of one
window, a higher z order is above a lower one (`webview_set_z_order`;
`LAUFEY_WEBVIEW_COMMAND_RAISE` / `LOWER` put a view above / below all the
others; equal ones keep their creation order). A new view starts on top.
`webview_state` reports `zOrder`. Engines without `LAUFEY_WEBVIEW_CAP_Z_ORDER`
keep the creation order and refuse the calls: CEF on Windows and Linux, whose
Views overlays are Aura windows CEF can't reorder (on macOS they are child
`NSWindow`s, reordered with `addChildWindow:ordered:`). WKWebView reorders the
subviews of the page view, WebView2 the host windows (`SetWindowPos`), WebKitGTK
the `GtkOverlay` children (`gtk_overlay_reorder_overlay`).

HTML can't draw over a web view. A page that shows a menu, dialog or another
element over one captures it (`webview_capture` with `scale` 1 and JPEG is the
cheapest), shows the image in its place, and hides the view
(`webview_set_visible(false)`) until the element is gone. On WKWebView the
capture takes what is on screen without waiting for a new frame
(`afterScreenUpdates = NO`) and encodes through ImageIO; CEF and WebView2 clip
`Page.captureScreenshot` to the visual viewport at the requested scale;
WebKitGTK resamples the snapshot with cairo.

## Zoom, find, DevTools, context menu, print and clipboard

Each is behind its `LAUFEY_WEBVIEW_CAP_*` bit; where it is not reported the call
returns false / `LAUFEY_WEBVIEW_UNSUPPORTED`. WebView2 and WebKitGTK report none
of them yet.

| Feature                       | CEF                                                            | WKWebView                                                                                |
| ----------------------------- | -------------------------------------------------------------- | ---------------------------------------------------------------------------------------- |
| Zoom (`ZOOM`)                 | `SetZoomLevel(log(factor) / log(1.2))`                         | `pageZoom` (macOS 11+)                                                                   |
| Find (`FIND`)                 | `CefBrowserHost::Find` + `CefFindHandler` (Chromium's count)   | `findString:withConfiguration:` (macOS 11+); count and active match approximated (below) |
| DevTools (`DEVTOOLS`)         | `ShowDevTools` (a DevTools window of its own)                  | the view's Web Inspector (`_inspector` `show`, as the window's `open_devtools`)          |
| Context menu (`CONTEXT_MENU`) | `OnBeforeContextMenu` (`model->Clear()` under `"app"`)         | a page-world `contextmenu` listener (`preventDefault` under `"app"`)                     |
| Print (`PRINT`)               | `CefBrowserHost::Print` (the OS dialog)                        | `printOperationWithPrintInfo:` as a sheet on the window (macOS 11+)                      |
| Clipboard (`CLIPBOARD`)       | `clipboardRead` answers Chromium's clipboard permission prompt | a page-world shim; `clipboardRead` also sets WebKit's clipboard preferences              |

- **Zoom.** On CEF the level is Chromium's, which follows the host within a
  request context: views of one partition showing the same host share it, and a
  navigation to another host takes that host's level. The view puts its own
  factor back after each main-frame navigation, so it keeps the factor it was
  given; another view of the partition on the same host changes with it.
- **Find.** CEF reports Chromium's own count and active match (the final update
  of each search). WKWebView's find says only whether there was a match; laufey
  counts the case-folded (or, with `MATCH_CASE`, exact) occurrences of the text
  in `document.body.innerText` in a content world of its own, and tracks the
  active match from the calls: a new text starts at 1 (forward) or the last
  match (backward), the same text again moves one on and wraps. Text WebKit
  finds that `innerText` doesn't show (hidden text, frames, shadow roots) or
  folds differently (WebKit also folds some characters, such as quote marks)
  makes the count and index approximate; a match WebKit found is always reported
  as at least one. `stop_find` clears the page's selection (the match WebKit
  selected).
- **DevTools.** `webview_open_devtools` answers `LAUFEY_WEBVIEW_NOT_ALLOWED`
  whenever the app's DevTools are off (`LAUFEY_INSPECTABLE=0` /
  `"inspectable": false`); WKWebView's `inspectable` (and
  `developerExtrasEnabled` before macOS 13.3) follow the same switch. On CEF a
  call before the browser exists opens them once it does.
- **Context menu.** `CONTEXT_MENU` fires for every request in both modes; `x` /
  `y` are in the view's CSS pixels (WKWebView: the event's `clientX` / `clientY`
  times `pageZoom`). On WKWebView the page-world listener runs before the page's
  scripts, ignores untrusted events and posts to a message handler that only the
  main frame's messages reach, checked field by field and capped at 20 a second;
  still, the page can see it, post a fake request to the handler, or cancel a
  native menu itself. A `contextmenu` in a subframe is not reported (`"app"`
  still suppresses its menu). CEF's comes from the browser process: the page
  can't fake one, but it can still cancel it (and then nothing is reported), as
  in any browser.
- **Print** returns once the dialog started; the dialog runs on its own.
- **Clipboard.** CEF: `clipboardRead` decides Chromium's clipboard permission
  prompt (`CEF_PERMISSION_TYPE_CLIPBOARD`, accepted only with it, never shown);
  `clipboardWrite: false` is **not applied**, as CEF has no switch for it: a
  page may write on a user gesture, as in Chrome. WKWebView: unless
  `clipboardRead`, a script in every frame replaces `navigator.clipboard`'s
  `read` / `readText` with ones that reject (`NotAllowedError`); with
  `clipboardWrite: false` it does the same to `write` / `writeText`, makes
  `document.execCommand('copy' | 'cut')` return false and stops the page's own
  `copy` / `cut` handlers (the user's own copy of a selection still copies it).
  With `clipboardRead` the view also sets WebKit's
  `javaScriptCanAccessClipboard` / `DOMPasteAllowed` preferences, so reads work
  without WebKit's Paste callout. The shim is best effort: a frame it does not
  reach, or a page that finds another realm's untouched clipboard, gets around
  it.

## Keys and focus

While a web view has the keyboard focus, its keys go in this order:

1. **The app menu's accelerators** (the app menu, the window's menu bar and its
   Window items: ⌘W, ⌘T, ⌘N, ...). A key matching one fires the item and the
   page never sees it, as in a browser (`LAUFEY_WEBVIEW_CAP_MENU_ACCELERATORS`).
2. **`forwardKeys`**: accelerators in the menu / global shortcut syntax
   (`"CmdOrCtrl+1"`, `"Meta+Shift+L"`; `CmdOrCtrl` is Command on macOS, Ctrl
   elsewhere; at most 64 entries of at most 64 characters, an invalid one is
   `LAUFEY_WEBVIEW_INVALID`), set at creation or with
   `webview_set_forward_keys`. A matching keydown skips the page and comes as a
   `KEY` event with `"forwarded": true` (`LAUFEY_WEBVIEW_CAP_FORWARD_KEYS`).
   Keys are matched by the physical key (DOM `code`), as accelerators are.
3. **The page.** A keydown it did not handle (not `preventDefault`ed, no
   editable used it) comes afterwards as a `KEY` event with `"forwarded": false`
   where `LAUFEY_WEBVIEW_CAP_UNHANDLED_KEYS` is reported.

`KEY` carries DOM `KeyboardEvent` names (`key`, `code`) and `altKey` / `ctrlKey`
/ `metaKey` (Command on macOS, the Windows / Super key elsewhere) / `shiftKey` /
`repeat`, mapped by the same tables as the window's own keyboard events
(keymap_mac.mm, keymap_vk.cc, keymap_gdk.cc).

A mouse-down in a view that does not have the focus reports `FOCUS`
`{"focused":true}` before the page has the click, and the click still reaches
the page (no lost first click).

| Engine    | Menu accelerators                                                                                                                         | `forwardKeys`                                                                                                                                                                                                                  | Unhandled keys                                                                                                                                            | Focus before the click                                                                                                                         |
| --------- | ----------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| WKWebView | the view's `performKeyEquivalent:` / `keyDown:` ask `[NSApp.mainMenu performKeyEquivalent:]` first                                        | the same overrides, after the menu                                                                                                                                                                                             | WebKit re-sends a keydown the page did not handle as the same `NSEvent` through `NSApp`; the view seeing it again reports it                              | `mouseDown:` makes the view the first responder before WebKit has the click; `acceptsFirstMouse:` is YES                                       |
| CEF       | `OnPreKeyEvent`: macOS `[NSApp.mainMenu performKeyEquivalent:]` with the `os_event`; Windows / Linux the window's Views menu accelerators | `OnPreKeyEvent`, after the menu                                                                                                                                                                                                | `OnKeyEvent` (Chromium's unhandled keyboard events)                                                                                                       | `OnGotFocus`: Chromium focuses the render widget on a press before forwarding it to the renderer                                               |
| WebView2  | the child controller's `AcceleratorKeyPressed`, matched against the window's menu accelerators as the page's is                           | `AcceleratorKeyPressed`, which WebView2 raises only for Ctrl / Alt combinations, keys that type no character and Escape: a forward key that types a character without Ctrl or Alt (`Shift+L`, `Meta+L`) still reaches the page | not reported (WebView2 says nothing of the page's handling)                                                                                               | the host window's `WM_PARENTNOTIFY` for a button press in the web view's windows, as the press is dispatched; best effort (`GotFocus` follows) |
| WebKitGTK | GtkWindow activates the window's accelerators (the menu bar's `GtkAccelGroup`) before the focus widget has the key                        | the view's `key-press-event` handler, run before WebKit's                                                                                                                                                                      | WebKit re-sends an unhandled keydown through `gtk_main_do_event` (a copy of the event: same time, keycode, state); the handler seeing it again reports it | the view's `button-press-event` handler, before WebKit's, takes the focus                                                                      |

The unhandled-key paths on WKWebView and WebKitGTK rest on how WebKit hands an
unhandled key back to the toolkit (WebKit's `doneWithKeyEvent`), which no API
documents. On WebView2 the order of `FOCUS` and the page's click is not
guaranteed: `WM_PARENTNOTIFY` only reaches the host when the web view's own
windows let it, and `GotFocus` is raised asynchronously.
