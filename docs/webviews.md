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
- **None of the window's keyboard events.** Keys typed into a view are its
  page's: they never reach the window's keyboard handler (they may be a
  third-party page's passwords). Only the app menu's accelerators, the view's
  `forwardKeys` matches and the keys its page didn't handle reach the embedder,
  the last two as the view's KEY events. WKWebView's window-wide key monitor and
  WebKitGTK's window key handler skip a focused child view; CEF and WebView2
  give a child view's keys to its own browser only.
- **None of the window's mouse events.** Clicks, moves and scrolls over a shown
  view are its page's: the window's mouse handlers never see them. WKWebView's
  window-wide monitors hit-test the event against the child views, WebKitGTK's
  window handlers check the event's widget, and CEF's mouse monitors on Windows
  (a thread hook) and Linux (XI2) skip points over a shown overlay
  (`CoversScreenPixel`). On CEF for macOS each overlay is a window of its own,
  and WebView2 delivers a view's input to its own window only. The view still
  reports FOCUS before the first click reaches its page.
- **None of the window's cursor.** Over a shown view only its page sets the
  cursor; over a hidden one the window's page does. A WKWebView child is a
  subview of the window's page view, whose WebKit tracking area covers it, so
  the page view drops the mouse moves that hit-test into a shown child (as
  WebKit drops those over a view stacked above it); a drag that starts in the
  window's page is still the page's. A WebKitGTK view has a window of its own,
  whose cursor is the only one shown over it, and Chromium (CEF, WebView2) sets
  a page's cursor only while the page's own view is under the pointer.
- **None of the window's navigation rules.** The window's navigation guard and
  will-navigate handler never see the view; its main frame may load http(s)
  pages and `about:blank`, and any other scheme (`file:`, `data:`,
  `javascript:`, the app's custom schemes, `mailto:`) is cancelled.

## Popups, downloads and permissions

| Option        | Values                                                                                                                                                                      |
| ------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `allowPopups` | `"same-view"` (default): the popup's URL loads in the view. `"browser"`: it opens in the OS browser. `"deny"`: nothing opens and a `NEW_WINDOW` event says what was asked.  |
| `downloads`   | `"deny"` (default), `"ask"` (a save panel; WebView2's own download UI), `"browser"` (the URL opens in the OS browser), `"app"` (API 59: the app decides, below).            |
| `permissions` | `"deny"` (default: refused without a prompt), `"ask"` (the engine's prompt, or a message dialog naming the origin on CEF and WebKitGTK), `"app"` (API 59: the app answers). |

No second view is ever created for a popup: the page's `window.open()` returns
`null`.

## Downloads the app decides (API 59)

With `"downloads": "app"` (`LAUFEY_WEBVIEW_CAP_DOWNLOAD_EVENTS`) the view shows
no download UI and writes nothing by itself. Each download is a series of
`DOWNLOAD` events with one `id`:

```rust
use laufey::{DownloadAction, WebViewEventKind};

let view = create_webview(window.id(), &options, move |event| {
  if event.kind == WebViewEventKind::Download {
    // {"id": 7, "state": "requested", "url": "...", "filename": "report.pdf",
    //  "mimeType": "application/pdf", "totalBytes": 48213, "receivedBytes": 0}
    println!("{}", event.detail_json);
  }
})?;
view.download_control(7, DownloadAction::Save("/Users/me/Downloads/report.pdf"))?;
```

- `"requested"` is the first event. The download waits until the app answers
  with `download_control`: `Save(path)` (an absolute path whose directory
  exists; a regular file there is replaced; a directory or a symbolic link is
  refused), `SaveAs` (the OS save panel, as `"ask"`; dismissing it cancels the
  download) or `Cancel`.
- `"progressing"` then repeats, at most five a second per download, with
  `receivedBytes`, `totalBytes` where known and `path`.
- `"completed"`, `"cancelled"` and `"interrupted"` end it. An interrupted
  download whose `canResume` is true waits for `Resume` or `Cancel`.
- `Pause` / `Resume` work where `LAUFEY_WEBVIEW_CAP_DOWNLOAD_PAUSE` is reported.
- `filename` is always a bare, sanitized file name (`SanitizeDownloadFileName`):
  the page and the server only propose a name, never a path.
- Closing the view cancels its downloads; their `"cancelled"` events come before
  `CLOSED`. At most `LAUFEY_WEBVIEW_MAX_PENDING_DOWNLOADS` (16) may wait for an
  answer in one view; one more is cancelled without an event.
- A download that did not complete leaves no file at its path.

An engine without the capability refuses the option (`LAUFEY_WEBVIEW_INVALID`)
rather than fall back to its default.

| Engine    | Events from                                                                                           | Pause / resume                                                                              | Notes                                                                                                                                                                                                                                    |
| --------- | ----------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| CEF       | `CefDownloadHandler` (`OnBeforeDownload`, `OnDownloadUpdated`)                                        | yes (`CefDownloadItemCallback::Pause` / `Resume`); `canResume` follows the interrupt reason | Chromium starts receiving into a temporary file of its own while the download waits for the app, and deletes it on cancel; nothing is written to a path the app did not choose. A cancel after `Save` also removes what was at the path. |
| WKWebView | `WKDownloadDelegate` (macOS 11.3+); progress from the download's `NSProgress`                         | no: a `WKDownload` can only be cancelled with resume data, which a server may not honour    | `Pause` / `Resume` answer `UNSUPPORTED`; `canResume` is always false.                                                                                                                                                                    |
| WebView2  | `ICoreWebView2_4::DownloadStarting` (held with a deferral), `ICoreWebView2DownloadOperation`'s events | yes (`Pause` / `Resume`, `CanResume`)                                                       | WebView2's own download UI never shows (`Handled`).                                                                                                                                                                                      |
| WebKitGTK | `WebKitDownload`'s `decide-destination`, `received-data`, `failed`, `finished` (WebKitGTK 2.40+)      | no (WebKitGTK has no pause)                                                                 | `Pause` / `Resume` answer `UNSUPPORTED`; `canResume` is always false.                                                                                                                                                                    |

WebView2 pauses a download asynchronously (`Pause` interrupts it with
`USER_PAUSED` a moment later, and a `Resume` before that does nothing): a
`RESUME` that comes before the pause has taken effect is applied once it has, so
a pause followed at once by a resume leaves the download running.

WebView2 has been seen to raise a download's `StateChanged` 20 s or more late,
or not at all, on a loaded machine. So once a download is saved (or resumed),
laufey also reads its state every second, until 5 minutes after its last sign of
life (the save, a resume, received bytes), and reports a completion or an
interruption `StateChanged` has not: each event still goes once, whichever sees
it first. A view with `"downloads": "app"` also turns WebView2's SmartScreen
reputation check off (`ICoreWebView2Settings8::IsReputationCheckingRequired`,
where the runtime has it), as the app decides each download; WebView2 has that
setting per web view only, so the view's navigations go unchecked too.

## Permission requests the app answers (API 59)

With `"permissions": "app"` (`LAUFEY_WEBVIEW_CAP_PERMISSION_REQUESTS`) the
engine's own prompt never shows. Each request is a `PERMISSION_REQUEST` event:

```json
{
  "id": 3,
  "state": "requested",
  "origin": "https://meet.example",
  "permissions": ["camera", "microphone"],
  "frame": { "isMain": false, "origin": "https://embed.example" }
}
```

`permission_respond(id, allow)` allows or refuses everything the request asked
for. A request that goes away unanswered (the page navigates, the engine
withdraws it, the view closes) is refused and reported with
`{"id": 3, "state": "cancelled"}`. A kind laufey cannot name is refused without
an event, and at most `LAUFEY_WEBVIEW_MAX_PENDING_PERMISSIONS` (16) may wait per
view. An allowed capture still needs the OS's own consent for the app (on macOS
the camera / microphone prompt and the bundle's usage strings).

| Engine    | Kinds it can raise                                                                                                                                                                   | `frame`                                                                                    | What the engine remembers                                                                                                                                                                         |
| --------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| CEF       | camera, microphone, screen, geolocation, notifications, midi, pointer-lock, storage-access, sensors, window-management, local-fonts, file-system, multiple-downloads                 | for camera, microphone and screen (`OnRequestMediaAccessPermission`); not for the others   | Chromium stores an allow or a refusal of a prompted kind per origin in the view's partition, so the same origin may not be asked again there; camera, microphone and screen are asked every time. |
| WKWebView | camera, microphone (macOS 12+). WebKit refuses the others without asking the embedder.                                                                                               | yes (`initiatedByFrame`)                                                                   | WebKit may reuse an answer for the same document.                                                                                                                                                 |
| WebView2  | camera, microphone, geolocation, notifications, sensors, clipboard-read, multiple-downloads, file-system, autoplay, local-fonts, midi, window-management. Screen capture is refused. | yes, where every frame took the frame-level handler (`ICoreWebView2Frame3`); else left out | nothing where the runtime has `SavesInProfile` (set to false); an older runtime may save the answer in the partition's profile.                                                                   |
| WebKitGTK | camera, microphone, screen, geolocation, notifications, pointer-lock, storage-access, clipboard-read (2.42+)                                                                         | no; `origin` is the top-level page's (for storage-access, the requesting domain)           | the web process keeps a notification answer per origin while it lives, and geolocation / capture answers per document.                                                                            |

Clipboard reads stay under the `clipboardRead` option where the engine applies
it (CEF, WKWebView).

## Events

Every event carries a JSON object; `CLOSED` is the last one and fires exactly
once, after `webview_close` or when the window closes. See the event kinds in
`laufey.h` (`DOWNLOAD` and `PERMISSION_REQUEST` are described above). `CONSOLE`
events are capped at 200 per second per view; on WKWebView and WebKitGTK they
come from a page-world script (WebKit reports no console messages to the
embedder), which the page could also post to. On WebKitGTK that script also
tells a refused popup's `NEW_WINDOW` disposition (`"popup"` when `window.open()`
was given a size or position), since WebKitGTK reports window features only to a
view it would create.

## Evaluate and capture

`webview_evaluate` runs a classic script in an isolated world of the main frame
named `laufey-webview`: it shares the DOM, not the page's globals (a page
variable, a page function or a prototype the page patched is not seen). Its
completion value comes back as JSON, a promise awaited. No page
Content-Security-Policy refuses it:

- **CEF and WebView2** use DevTools (`Page.createIsolatedWorld`,
  `Runtime.evaluate` with `allowUnsafeEvalBlockedByCSP`;
  `CallDevToolsProtocolMethod` on WebView2).
- **WKWebView and WebKitGTK** run a wrapper through `callAsyncJavaScript` in a
  `WKContentWorld` / `webkit_web_view_call_async_javascript_function` in a
  script world (WebKitGTK 2.40+; older versions report no
  `LAUFEY_WEBVIEW_CAP_EVALUATE`), which evaluates the script with an indirect
  `eval`. WebKit applies the page's CSP to `eval` in every world, so the wrapper
  first tries `eval('0')`: when the CSP refuses it, nothing of the script has
  run, and the script runs again through the engine's own evaluate
  (`evaluateJavaScript:inFrame:inContentWorld:`, macOS 11+;
  `webkit_web_view_evaluate_javascript`), in the same world, which no page CSP
  restricts. That path returns the completion value as it is: a promise is not
  awaited (`NOT_SERIALIZABLE`), so under such a CSP end the script with a plain
  value. A page with a CSP `report-uri` gets one report for the refused
  `eval('0')` per call.

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

1. **`forwardKeys` entries marked `!`** (`"!CmdOrCtrl+W"`;
   `LAUFEY_WEBVIEW_CAP_FORWARD_KEYS_BEFORE_MENU`): a matching keydown skips the
   menu and the page, as a forwarded `KEY` event. Use it for a key the view's
   app means while the view has the keyboard (its own ⌘W, say), which the menu
   would otherwise take.
2. **The app menu's accelerators** (the app menu, the window's menu bar and its
   Window items: ⌘W, ⌘T, ⌘N, ...). A key matching one fires the item and the
   page never sees it, as in a browser (`LAUFEY_WEBVIEW_CAP_MENU_ACCELERATORS`).
3. **`forwardKeys`**: accelerators in the menu / global shortcut syntax
   (`"CmdOrCtrl+1"`, `"Meta+Shift+L"`; `CmdOrCtrl` is Command on macOS, Ctrl
   elsewhere; at most 64 entries of at most 64 characters, an invalid one is
   `LAUFEY_WEBVIEW_INVALID`; a leading `!` moves the entry to step 1), set at
   creation or with `webview_set_forward_keys`. A matching keydown skips the
   page and comes as a `KEY` event with `"forwarded": true`
   (`LAUFEY_WEBVIEW_CAP_FORWARD_KEYS`). Keys are matched by the physical key
   (DOM `code`), as accelerators are.
4. **The page.** A keydown it did not handle (not `preventDefault`ed, no
   editable used it) comes afterwards as a `KEY` event with `"forwarded": false`
   where `LAUFEY_WEBVIEW_CAP_UNHANDLED_KEYS` is reported.

**Keyups** (`LAUFEY_WEBVIEW_CAP_KEY_UP`): the keyup of a forwarded keydown comes
as a `KEY` event with `"type": "keyup"` and `"forwarded": true`, matched by the
physical key whatever modifiers are down by then (Ctrl released before the W of
Ctrl+W still ends it), and the page never sees it. A held key's repeats are
forwarded keydowns with `"repeat": true`. When the view loses the focus (a click
elsewhere, `BLUR`, hiding it, the window or the app deactivated) with forwarded
keys still down, their keyups come then, so a hold always ends. Keyups of keys
that were not forwarded are the page's.

**Giving the keyboard back.** `webview_command(BLUR)`
(`LAUFEY_WEBVIEW_CAP_BLUR`) moves the focus from the view to the window's page
if the view has it, and reports `FOCUS` `{"focused":false,"reason":"blur"}`; the
app is not activated and no window is raised or reordered (`FOCUS` the command,
by contrast, brings the window forward). Hiding a focused view
(`webview_set_visible(false)`) does the same with `"reason":"hidden"`. A `FOCUS`
change laufey did not make has no `"reason"`.

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

| Engine    | `!` entries                                                                            | Keyups                                                                                                                                                       | `BLUR` / hiding                                                                                                             |
| --------- | -------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------- |
| WKWebView | `performKeyEquivalent:` / `keyDown:`, before `[NSApp.mainMenu ...]`                    | a local `keyUp` monitor (AppKit sends no `keyUp:` while Command is down); the app's deactivation releases the rest                                           | the page view becomes the window's first responder (`makeFirstResponder:`; no `makeKeyWindow`)                              |
| CEF       | `OnPreKeyEvent`, before the menu                                                       | `OnPreKeyEvent`'s `KEYEVENT_KEYUP`; on macOS a local `keyUp` monitor and the app's deactivation, as on WKWebView                                             | `SetFocus(false)`, the page's browser view `RequestFocus()`; on macOS the parent window made key again when the overlay was |
| WebView2  | `AcceleratorKeyPressed`, before `HandleAcceleratorKey`                                 | `AcceleratorKeyPressed`'s `KEY_UP`, and, as WebView2 raises it only for accelerator keys, a host timer reading the key's state every 30 ms while one is held | the page controller's `MoveFocus`, in the foreground window only                                                            |
| WebKitGTK | the window's own `key-press-event` handler, which runs before GtkWindow's accelerators | the view's `key-release-event` handler                                                                                                                       | `gtk_widget_grab_focus` on the page view (it never presents the window)                                                     |

The unhandled-key paths on WKWebView and WebKitGTK rest on how WebKit hands an
unhandled key back to the toolkit (WebKit's `doneWithKeyEvent`), which no API
documents. On WebView2 the order of `FOCUS` and the page's click is not
guaranteed: `WM_PARENTNOTIFY` only reaches the host when the web view's own
windows let it, and `GotFocus` is raised asynchronously.
