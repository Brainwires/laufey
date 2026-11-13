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

| Backend                     | Engine                                                                                  | Capabilities                                                                                                         |
| --------------------------- | --------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------- |
| CEF                         | An Alloy-style `CefBrowserView` added with `CefWindow::AddOverlayView` (custom docking) | everything; persistent partitions when the app's profile is on disk                                                  |
| WebView, macOS              | A second `WKWebView`, a subview of the window's page view                               | everything; persistent partitions on macOS 14+ (`WKWebsiteDataStore` per partition), permission prompts on macOS 12+ |
| WebView, Windows (WebView2) | Not yet (`webview_capabilities` is 0)                                                   |                                                                                                                      |
| WebView, Linux (WebKitGTK)  | A second `WebKitWebView`, a child of the `GtkOverlay` that holds the window's page view | everything; persistent partitions when the app has a data directory, evaluate on WebKitGTK 2.40+                     |
| Winit                       | None (the entry points are NULL)                                                        |                                                                                                                      |

`webview_capabilities` reports `LAUFEY_WEBVIEW_CAP_*` and the engine (bits
24–27). On a backend without web views `create_webview` answers
`LAUFEY_WEBVIEW_UNSUPPORTED`.

## Isolation

A web view is a browsing context of its own:

- **No bridge.** Its pages get no injected bridge or external-link script, no
  JS-call handler hears them, and no binding or custom-scheme handler is
  reachable from them. On CEF it has its own `CefClient`, which drops every
  process message its renderer sends, and the renderer is told (through the
  browser's `extra_info`) never to install the bridge.
- **Its own storage.** `partition` names a storage profile (cookies,
  localStorage, IndexedDB, cache) shared by the views with the same name and
  never the app's own: a `CefRequestContext` per name under
  `<profile>/WebViews/<name>` on CEF, a `WKWebsiteDataStore` for a UUID derived
  from the name on macOS 14+ (`PartitionUuid`), a `WebKitWebContext` over a
  `WebKitWebsiteDataManager` under `<app data dir>/WebViews/<name>` on
  WebKitGTK. `"ephemeral"` or `"ephemeral:<name>"` is kept in memory; so is
  every partition where `LAUFEY_WEBVIEW_CAP_PERSISTENT_PARTITIONS` is not
  reported (macOS before 14, a CEF app with an in-memory profile, a WebKitGTK
  app without a data directory: no `LAUFEY_DATA_DIR` / `LAUFEY_APP_ID`). The
  default is `"webview:default"`.
- **None of the window's navigation rules.** The window's navigation guard and
  will-navigate handler never see the view; its main frame may load http(s)
  pages and `about:blank`, and any other scheme (`file:`, `data:`,
  `javascript:`, the app's custom schemes, `mailto:`) is cancelled.

## Popups, downloads and permissions

| Option        | Values                                                                                                                                                                     |
| ------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `allowPopups` | `"same-view"` (default): the popup's URL loads in the view. `"browser"`: it opens in the OS browser. `"deny"`: nothing opens and a `NEW_WINDOW` event says what was asked. |
| `downloads`   | `"deny"` (default), `"ask"` (a save panel), `"browser"` (the URL opens in the OS browser).                                                                                 |
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
JSON. CEF uses DevTools (`Page.createIsolatedWorld`, `Runtime.evaluate`), which
no page Content-Security-Policy restricts. WKWebView runs it in a
`WKContentWorld` through an indirect `eval`, which a page CSP without
`'unsafe-eval'` may refuse (the call then fails with `SCRIPT_ERROR`). WebKitGTK
does the same in a script world named `laufey-webview`
(`webkit_web_view_call_async_javascript_function`, WebKitGTK 2.40+; older
versions report no `LAUFEY_WEBVIEW_CAP_EVALUATE`).

`webview_capture` encodes what the view shows as PNG or JPEG, in device pixels
(CEF: `Page.captureScreenshot`; WKWebView: `takeSnapshotWithConfiguration`;
WebKitGTK: `webkit_web_view_get_snapshot` of the visible region).

## Stacking

A web view is native and draws above the window's page. A page that shows a menu
or dialog over it hides the view (`webview_set_visible(false)`), or captures it
into an image shown in its place first.
