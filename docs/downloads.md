# Downloads

Pages in a laufey webview can download files the same way they would in a
browser: clicking an `<a download>` link, navigating to a response with
`Content-Disposition: attachment`, or saving a `Blob` the page created (a
generated CSV, an export, an image). A webview has no download UI of its own, so
the host is responsible for deciding where the bytes go. laufey asks the user
with the operating system's own "Save As" dialog:

```html
<button id="export">Export report</button>
<script>
document.getElementById("export").addEventListener("click", () => {
  const blob = new Blob(["report contents"], { type: "text/plain" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url;
  a.download = "report.txt";
  a.click();
  URL.revokeObjectURL(url);
});
</script>
```

The dialog is seeded with the file name the page (or the server's
`Content-Disposition` header) suggests. Dismissing it cancels the download —
nothing is written. The browser engine streams the bytes to the chosen path
itself, so file contents never pass through the runtime, and a large file is
never held in memory.

The dialog is the backend's own file dialog — the one `show_file_dialog` shows
(see [File dialogs](file-dialogs.md)), modal to the window:

| Backend         | Dialog                                                           | Engine hook                                     |
| --------------- | ---------------------------------------------------------------- | ----------------------------------------------- |
| webview/macOS   | `NSSavePanel` (a sheet)                                          | `WKDownloadDelegate` (macOS 11.3+)              |
| webview/Windows | `IFileSaveDialog`                                                | WebView2 `DownloadStarting` (`ICoreWebView2_4`) |
| webview/Linux   | xdg-desktop-portal's FileChooser, else `GtkFileChooserNative`    | WebKitGTK `decide-destination` (2.40+)          |
| CEF             | the same per OS (`NSSavePanel`, `IFileSaveDialog`, portal / GTK) | `CefDownloadHandler`                            |

The Winit backend has no web engine and never downloads anything.

## What a page can and cannot do

The save path comes only from the dialog. Before the dialog is shown at all,
every backend checks the download the same way
(`backend-common/include/laufey_downloads.h`):

- **Only the app's origins.** The download's URL — before and after any server
  redirect — must be on an origin the window's top-level frame may load: the
  origin the embedder navigated the window to, one of the embedder's allowed
  navigation origins (`set_allowed_navigation_origins`), or `file:` for a window
  showing a file. A `blob:` URL counts as the origin that created it. So a
  third-party iframe the page's CSP lets in (an embed, an ad, a payment form)
  can't start a download, neither by exporting a blob of its own nor by loading
  an attachment, and a link to another origin's file is not saved in-app (a
  top-level navigation there goes to the OS browser instead, as the navigation
  guard does for every other origin). An app that downloads from another origin
  in-app, such as a storage bucket its API redirects to, lists that origin in
  its allowed navigation origins.
- **The frame that started it.** Where the engine reports it (WebKit on macOS:
  `WKDownload.originatingFrame`), that frame must be on such an origin too.
  `data:` URLs have no origin of their own: they need such a frame, and are
  allowed where the engine doesn't say (CEF, WebView2, WebKitGTK); the dialog
  still asks.
- **A name, never a path.** The proposed name is the engine's suggestion reduced
  to a bare file name (`SanitizeDownloadFileName`): no directory part
  (`../../x`, `C:\x`), no control characters, bidirectional overrides or invalid
  UTF-8, none of `<>:"|?*`, no leading dots (no hidden files) or trailing dots /
  spaces, no Windows device names (`CON`, `NUL.txt`, …), at most 200 bytes. The
  dialog opens in its usual directory, not one the page names.
- **No silent overwrite.** The dialog asks before replacing a file. Only after
  that answer is an existing file at the chosen path replaced (WebKit on macOS
  refuses to write over one, so laufey removes the regular file there first;
  never a directory).
- **One dialog at a time, a few per window.** Downloads share the one-at-a-time
  file-dialog slot with the app's own dialogs: a download that arrives while a
  file dialog is open is cancelled. A window gets at most 3 save dialogs in any
  10 seconds; further downloads are cancelled without a dialog. A page looping
  downloads can't keep the dialog up.

A refused download is cancelled and logged on stderr
(`laufey: not downloading <url>: …`); nothing is written.

## Downloads from the app's own scheme

An app served by its own scheme handler (`myapp://app`, see
[Custom schemes](custom-schemes.md)) downloads the same way: an attachment it
serves, at the top level or in a frame, and an `<a download>` link to one of its
URLs ask with the same dialog, under the same rules. An attachment counts
whatever its type, as in a browser: `Content-Disposition: attachment` on a
`text/plain` or `text/html` response saves it instead of showing it, on every
backend.

The WebKit engines can't do this on their own. They make a navigation into a
download by handing its load to their network process, which never had a load
the scheme handler served, so the download never starts. On WKWebView and
WebKitGTK laufey cancels such a navigation (the page stays) and, once the dialog
has a path, asks the app's scheme handler for the URL again with a `GET` (the
original request's headers included) and writes the response itself: to a
temporary file next to the chosen path, which replaces it only when the whole
`2xx` response has arrived. So:

- the handler sees the URL twice (the navigation, then the save);
- only a `GET` (or `HEAD`) response is saved: an attachment answering a `POST`
  form is refused and logged
  (`a POST response from the app's scheme
  can't be saved`), since saving it
  would send the form again. Serve the file from a `GET`, for example by
  redirecting the form to it;
- an `<a download="name">` link's own name isn't visible to the WebKit backends
  here: the dialog proposes the response's `Content-Disposition` name, else the
  URL's last path segment.

CEF and WebView2 save such downloads through their own download path.

## Per-engine notes

- **CEF**: CEF never gets `Continue()` without a path (with no path it would
  save into the Downloads folder unasked). A refused or dismissed download is
  cancelled: its callback is released and `Cancel()` is called on its next
  update. `CanDownload` refuses another origin's download before it starts.
- **WebView2**: WebView2's own download UI never shows. The event is held open
  with a deferral while the dialog (on laufey's I/O thread) is up; the answer is
  applied on the UI thread. A runtime without `ICoreWebView2_4` keeps WebView2's
  default handling.
- **WebKitGTK**: the destination is decided asynchronously, which needs
  WebKitGTK 2.40; an older WebKitGTK cancels page downloads rather than save
  them unasked (it would otherwise save into the Downloads folder).
- **WKWebView**: needs macOS 11.3 (`WKDownload`); earlier, downloads stay inert.

## Testing

`scripts/native-e2e-run.sh <webview|cef> --downloads`
(`examples/native_e2e/src/download_checks.rs`) drives real downloads from a
loopback server and answers each save dialog with `test_file_dialog_respond`: a
blob `<a download>` export, an attachment whose filename is
`../../laufey-dl-evil-….txt` (only the chosen path is written; nothing appears
under the name the server proposed), a dismissed dialog (nothing written, the
Downloads folder included), a cross-origin iframe exporting a blob and loading
an attachment (no dialog), a loop of 8 downloads (at most 3 dialogs), a 32 MiB
attachment (arrives whole), and from a custom-scheme origin (`laufey-e2e://dl`)
a `text/plain` attachment at the top level, an `application/octet-stream`
attachment in a frame and an `<a download>` link (each asks and saves; nothing
appears under the response's own name). `backend-common/tests/downloads_test.cc`
covers the name sanitizing, the origin rules, the prompt budget, the
`Content-Disposition` parsing and the custom-scheme save's file handling.

## Fork note

Carried from upstream
[littledivy/laufey#126](https://github.com/littledivy/laufey/pull/126) (by
@ftaddia, head `cef9c88`), which added the engine hooks with each platform's
save dialog. This fork then routed every download through the backend's own file
dialog (one at a time, test-driveable), added the origin check, the name
sanitizing, the prompt budget, CEF's never-unasked cancel path, the WebView2
deferral, WebKitGTK's asynchronous `decide-destination` (the PR's handler
returned no value from a `gboolean` signal) and the e2e battery above. There is
no C ABI change: an app gets the behavior with no code.
