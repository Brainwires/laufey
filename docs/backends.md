# Backends

A backend is the native executable that hosts a browser (or windowing) engine
and implements the [C ABI](c-abi.md). laufey ships three; a fourth is on a
branch. All implement the same `laufey_backend_api_t`, so a runtime is portable
across them — the differences are in engine, process model, size, and a few
features that a given engine can't express on a given OS (see
[the feature pages](window-management.md)).

| Backend                                                           | Engine        | Process model | Bundled | JS bridge |
| ----------------------------------------------------------------- | ------------- | ------------- | ------- | --------- |
| [CEF](https://github.com/littledivy/laufey/tree/main/cef)         | Chromium 144  | multi-process | yes     | yes       |
| [WebView](https://github.com/littledivy/laufey/tree/main/webview) | system native | single        | no      | yes       |
| [Winit](https://github.com/littledivy/laufey/tree/main/winit)     | none          | single        | n/a     | no        |

Platform support is x86_64 + aarch64 on macOS and Linux, x86_64 on Windows.
There is also an **iOS** backend (UIKit + WKWebView, statically linked) — see
[iOS](ios.md). Android is not supported.

## CEF

Embeds Chromium 144 through the Chromium Embedded Framework and runs Chromium's
real multi-process architecture — a browser process plus renderer, GPU, and
utility subprocesses, with the same rendering and DevTools you get in Chrome.
The engine is bundled into the app, so binaries are large but rendering is
identical everywhere and independent of the host OS.

Sources live in [`cef/`](https://github.com/littledivy/laufey/tree/main/cef);
shared native features come from `backend-common`. On Windows the backend links
the static CRT (`/MT`), so everything it links — including `backend-common` — is
built `/MT`.

Linux caveat: the application menu doesn't work under CEF (a `GtkMenuBar` must
be packed into a GtkWindow above the browser, and reparenting CEF into a
client-owned GtkWindow via `CefWindowInfo::SetAsChild` breaks on XWayland).
Context menus do work, because `GtkMenu` popups need no GtkWindow container.

Custom schemes: Chromium registers them at process start, before the runtime is
loaded, so besides calling `register_scheme_handler` the embedder declares them
when launching the host — `--laufey-custom-schemes=myapp` or
`LAUFEY_CUSTOM_SCHEMES=myapp` (`cef/src/custom_schemes.h`). See
[Custom URL schemes](custom-schemes.md).

### The Chromium sandbox (off)

The CEF backend runs Chromium **without its sandbox**: every host sets
`CefSettings::no_sandbox = true` (`main_mac.mm`, `main_linux.cc`,
`main_windows.cc`) and the Makefile builds `libcef_dll_wrapper` with
`-DUSE_SANDBOX=OFF`. Renderer, GPU and utility processes therefore run with the
user's full rights, as the browser process does: a bug that gives a web page
code execution in its renderer is code execution as the user, where Chrome would
contain it. The WebView backends are not affected; WKWebView, WebView2 and
WebKitGTK sandbox their web content processes themselves.

Turning it on is per OS, and each needs build, packaging and signing work that
has to be verified on that OS. What CEF 149 (the version the Makefile pins)
requires:

- **macOS.** Build the wrapper with `USE_SANDBOX=ON` (it defines
  `CEF_USE_SANDBOX`). Only the helper apps are sandboxed, and laufey already
  ships them as separate executables (`laufey Helper*.app`,
  `cef/src/helper.cc`): each helper links the distribution's `cef_sandbox`
  library and initializes it (`CefScopedSandboxContext`) before it loads the
  framework. Then `no_sandbox = false` in `main_mac.mm`, and the whole bundle
  (the framework and every helper) code-signed with the hardened runtime and the
  entitlements Chromium's helpers need (JIT for the renderer helper). The
  renderer reads the custom-scheme list from `LAUFEY_CUSTOM_SCHEMES` /
  `laufey-launch.json` in every process (`custom_schemes.cc`); it would have to
  rely on the `--laufey-custom-schemes` switch the browser process forwards,
  since a sandboxed process can't be assumed to read files. This is the most
  direct of the three.
- **Linux.** `no_sandbox = false` in `main_linux.cc`, plus a working sandbox on
  the user's machine: either unprivileged user namespaces (on Ubuntu 23.10 and
  later, AppArmor restricts them, so the package must install an AppArmor
  profile granting `userns` to the executable), or the setuid `chrome-sandbox`
  helper the distribution ships (`Release/chrome-sandbox`), installed next to
  the executable owned by root with mode `4755`. Only a system package (a `.deb`
  / `.rpm` install step) can do either; a tarball or AppImage run by the user
  can't, and Chromium refuses to start ("No usable sandbox!") when neither
  works. So the host would also need to detect that and fall back to
  `--no-sandbox` with a warning, and that decision must stay out of reach of a
  deep-link launch's arguments (see [Deep links](deep-links.md)).
- **Windows.** In CEF 149 the Windows sandbox comes with the bootstrap model
  (`USE_SANDBOX=ON` defines `CEF_USE_BOOTSTRAP` there): the distribution's
  `bootstrap.exe` / `bootstrapc.exe` becomes the app's executable and loads the
  client code as a DLL; per CEF's bootstrap notes, a signed bootstrap executable
  only loads a client DLL signed with the same certificate (to be confirmed
  against the 149 distribution's README when this is done). laufey's CEF host is
  a single executable today (`WinMain` in `main_windows.cc`, which also finds
  the runtime library next to the executable and runs the headless worker mode);
  it would have to become that DLL, and the packaging scripts would ship and
  sign the bootstrap executable with it. Older CEF versions linked a
  `cef_sandbox.lib` into the client executable instead.

Until then, keep CEF windows on content the app serves itself (its custom
schemes) and open other sites in the user's browser, as laufey's external-link
handling does by default.

## WebView

Delegates to the platform's native web engine — **WKWebView** on macOS,
**WebView2** on Windows, **WebKitGTK** on Linux. The engine is never bundled, so
apps stay small, at the cost of rendering that varies by OS and engine version.
Single-process.

Sources live in
[`webview/`](https://github.com/littledivy/laufey/tree/main/webview), one file
per platform (`webview_macos.mm`, `webview_windows.cc`, `webview_linux.cc`),
sharing `backend-common` for menus, tray, dialogs, dock, and notifications.

Custom schemes registered through `register_scheme_handler` are installed per
engine — a `WKURLSchemeHandler` per scheme on the `WKWebViewConfiguration`,
`webkit_web_context_register_uri_scheme` plus the security manager's secure/CORS
flags on WebKitGTK, `CoreWebView2CustomSchemeRegistration` (TreatAsSecure,
HasAuthorityComponent) on the WebView2 environment — so each is a real
`<scheme>://<host>` origin. WKWebView and WebView2 read the set when a web view
(WebView2: the first one) is created; register schemes before the first window.
See [Custom URL schemes](custom-schemes.md).

## Winit

Engine-free. It creates native windows via
[winit](https://github.com/rust-windowing/winit) for apps that draw their own
content — GPU surfaces, custom renderers — without loading a web engine. There
is no JS bridge; `get_window_handle` / `get_display_handle` expose the raw
handles needed to create a rendering surface. Sources in
[`winit/`](https://github.com/littledivy/laufey/tree/main/winit).

## Servo (experimental)

A [Servo](https://servo.org)-based backend is preserved on the
[`servo`](https://github.com/littledivy/laufey/tree/servo) branch for future
work and is not part of the mainline build.

## backend-common

CEF and WebView share their native-API implementations (menus, tray, dock,
dialogs, notifications, key mapping) in
[`backend-common/`](https://github.com/littledivy/laufey/tree/main/backend-common),
included as a CMake subdirectory by each backend. The winit backend shares its
non-engine pieces through `backend-winit-common` instead.
