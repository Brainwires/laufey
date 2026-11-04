# Microphone and camera

A page reaches the microphone and the camera through `getUserMedia`. Every
request goes to the backend, which decides it exactly once and refuses it by
default (API 53). Screen and window capture (`getDisplayMedia`) is never
granted.

## Who may capture

1. A request is refused when it asks for nothing, for anything but the
   microphone and camera, comes from an opaque origin (`about:`, `data:`,
   `file:`), or comes from a sub-frame whose origin is not the window's
   top-level document's. Nobody is asked about such a request.
2. A request is allowed when the launch file's `mediaCapture` grants its origin
   every kind it asks for ([Launch configuration](launch-config.md)). The
   runtime is not asked.
3. Otherwise the runtime's handler is asked, and the request waits for its
   answer. With no handler, it is refused.

```json
{ "mediaCapture": { "microphone": ["myapp://app"], "camera": ["myapp://app"] } }
```

Entries are exact origins (`scheme://host[:port]`); `*` and `<scheme>://*` are
not accepted, so a grant names the app's own documents and nothing a window
navigates to. A page asking for both kinds needs both granted, or the handler is
asked about the request as a whole.

```rust
use laufey::{MediaCaptureKinds, MediaCaptureRequest};

laufey::on_media_capture_request(|req: MediaCaptureRequest| {
  let allow = req.origin == "myapp://app" && req.kinds == MediaCaptureKinds::MICROPHONE;
  laufey::answer_media_capture(req.id, allow);
});
```

The handler fires on the UI thread, and `answer_media_capture` may be called
from any thread, later (after asking the user, for example). Until then the
page's `getUserMedia` promise stays pending, as it would behind a browser's
prompt. A request is refused when its window is closed, when the window's
top-level document is replaced (its main frame commits a navigation to another
document; a fragment or `pushState` navigation keeps it), or when
`clear_media_capture_handler` removes the handler; an answer after that returns
`false`, so a late answer never grants the devices to whatever page the window
shows by then. When a yes is delivered, the backend checks once more that the
window's top-level document is still of the origin the request was made for, and
refuses it otherwise. Nothing is remembered between requests: each one meets the
launch file and the handler again.

## The engines

| Backend   | Hook                                                          | Asking frame                                                |
| --------- | ------------------------------------------------------------- | ----------------------------------------------------------- |
| WKWebView | `requestMediaCapturePermissionForOrigin` (macOS 12 and later) | the frame's security origin, and the web view's URL         |
| CEF       | `CefPermissionHandler::OnRequestMediaAccessPermission`        | the requesting origin and its frame, and the main frame     |
| WebView2  | `PermissionRequested` (`Microphone`, `Camera`), one per kind  | the frame's own committed origin, and the web view's source |
| WebKitGTK | `permission-request` (`WebKitUserMediaPermissionRequest`)     | not reported: the top-level document's origin is checked    |

A request's document is replaced at WKWebView's `didCommitNavigation`, CEF's
`OnLoadStart` for the main frame, WebView2's `ContentLoading` and WebKitGTK's
`load-changed` (`WEBKIT_LOAD_COMMITTED`), none of which fire for a same-document
navigation. The answer-time check reads the web view's URL (WKWebView,
WebKitGTK), its `Source` (WebView2) or the main frame's URL (CEF).

Chromium names the top-level document's origin for a sub-frame the device is
delegated to (`<iframe allow="microphone">`), so neither CEF nor WebView2 takes
the origin the engine reports on its own. CEF checks it against the asking
frame's URL. WebView2's frames have no URL and its web view event doesn't say
which frame asks, so every sub-frame's request is caught at the frame
(`FrameCreated`, `ICoreWebView2Frame3`'s `PermissionRequested`, handled there)
and decided with the origin of the document the frame last committed, from the
frame's own `NavigationStarting` and `ContentLoading`. A frame that hasn't
committed a document it saw start (its initial `about:blank`, a `srcdoc` frame),
or that has a child frame (WebView2 raises a descendant's request on the
ancestor it knows), is refused; so is every request in a window with a frame
whose requests can't be caught at the frame (an older runtime).

CEF never shows Chrome's own prompt, and WebView2's decisions are not saved in
the profile.

Screen capture never reaches the handler. CEF reports it in the same hook
(`DESKTOP_AUDIO_CAPTURE`, `DESKTOP_VIDEO_CAPTURE`) and WebKitGTK as a display
device request, and both are refused there; WKWebView refuses a display request
itself when the app has no display-capture delegate. WebView2 asks nobody: it
shows its own source picker, so laufey cancels its `ScreenCaptureStarting` event
(`ICoreWebView2_27`, raised for the main frame and every sub-frame), and the
page gets `NotAllowedError`. A WebView2 runtime older than 1.0.2903 lacks the
event and keeps the picker.

WebKitGTK does not say which frame asks, so there a sub-frame reaches the
devices only when the top-level document delegates them to it (Permissions
Policy: `<iframe allow="microphone">`), which WebKit enforces before it asks; on
the other backends a sub-frame of another origin is refused even then.
WebKitGTK's `enable-media-stream` setting is turned on so that
`navigator.mediaDevices` exists; requests are still refused unless granted.

## The operating system's consent

An allowed request still meets the OS's own consent, which laufey does not
manage:

- macOS asks the user once per app (TCC). The app's `Info.plist` must carry
  `NSMicrophoneUsageDescription` and / or `NSCameraUsageDescription`; without
  them macOS ends the process when the page first captures. A hardened-runtime
  build also needs the `com.apple.security.device.audio-input` and / or
  `com.apple.security.device.camera` entitlements. A refusal there reaches the
  page as `NotAllowedError`, like laufey's own.
- Windows has a per-user privacy switch for desktop apps' microphone and camera
  access.
- Linux has no consent of its own for these devices (a Flatpak's portal aside).

## Tests

`scripts/native-e2e-run.sh <backend> --media-capture` runs
`examples/native_e2e/src/media_capture_checks.rs` under a launch file granting
`app://e2e-media-ok`, with stand-in devices (the screen capture check needs
none): Chromium's `--use-fake-device-for-media-stream` (CEF on its command line,
WebView2 as a browser argument) and WebKit's mock capture devices, the latter
two through the `LAUFEY_TEST_MOCK_CAPTURE_DEVICES=1` test hook. On macOS an
unsigned test host may be refused by TCC, so an allowed request that ends in
`NotAllowedError` is reported N/A there. The decision logic is unit-tested in
`backend-common/tests/media_capture_test.cc`.
