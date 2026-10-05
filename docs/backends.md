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

### The Chromium sandbox

The CEF backend runs Chromium's renderer, GPU and utility processes in
Chromium's sandbox on macOS and Linux. On Windows it doesn't yet (see below).
The WebView backends are not affected: WKWebView, WebView2 and WebKitGTK sandbox
their web content processes themselves.

`--no-sandbox` on the host's command line still turns it off for a debugging
session; a deep-link launch drops that switch with every other Chromium switch
(see [Deep links](deep-links.md)).

- **macOS.** Every helper app (`laufey Helper*.app`, `cef/src/helper.cc`) enters
  the sandbox before it loads the framework: `CefScopedSandboxContext` loads the
  distribution's `libcef_sandbox.dylib` from the framework's `Libraries`
  directory and applies the Seatbelt profile the browser process passes for the
  helper's role (`main_mac.mm` sets `no_sandbox = false`). Nothing changes in
  packaging or signing: the helpers keep their entitlements
  (`cef/macos/entitlements-helper.plist`, JIT for V8), and the framework ships
  `libcef_sandbox.dylib` signed with it.
- **Linux.** Chromium runs a renderer under seccomp-bpf plus a layer-1 sandbox
  (its own PID, network and user namespaces), which comes from one of two
  places:
  - unprivileged user namespaces, where the kernel allows them. Ubuntu 23.10 and
    later restrict them with AppArmor
    (`kernel.apparmor_restrict_unprivileged_userns=1`) unless an AppArmor
    profile grants `userns` to the executable;
  - the setuid helper `chrome-sandbox` next to the executable, owned by root
    with mode 4755. Chromium uses it only when it can't create user namespaces.
    A system package can install it that way: Deno Desktop's `.deb` and `.rpm`
    do.

  With neither, Chromium refuses to start sandboxed ("No usable sandbox!"), and
  as root it refuses to start at all. So the host (`main_linux.cc`,
  `laufey_cef_sandbox.h`) probes the machine the way Chromium will, before
  `CefInitialize`: a child started in a new user namespace (a raw
  `clone(CLONE_NEWUSER)`, as Chromium does) that maps its ids and creates a
  nested one, and the helper's owner and mode. A helper can't gain root, and
  counts as unusable, when the process runs with `no_new_privs` (a container, a
  systemd unit with `NoNewPrivileges=`) or the executable sits on a `nosuid`
  mount. The host turns the sandbox off only when Chromium would have no usable
  sandbox, or as the root user; when the probe itself can't run (the child can't
  be started, say at `RLIMIT_NPROC`), it leaves the sandbox on and Chromium
  chooses. Every launch logs one line, the mode (`namespace`, `setuid`,
  `chromium` or `off`) and why:

  ```
  laufey: sandbox: off (unprivileged user namespaces are restricted by AppArmor (kernel.apparmor_restrict_unprivileged_userns=1), and there is no chrome-sandbox helper next to the executable; install the app from its .deb or .rpm package to run web content sandboxed)
  ```

  That is the case for a tarball or an AppImage (mounted `nosuid`) on Ubuntu
  23.10 and later. The decision rests on the machine and the installed files
  only, never on the command line (`CHROME_DEVEL_SANDBOX`, Chromium's override
  of the helper's path, can only make Chromium abort on a bad helper, never turn
  the sandbox off). The GPU process's seccomp sandbox is Chromium's own
  decision: Chromium skips it when the GL driver started threads before the
  sandbox, as Mesa's llvmpipe (software GL, e.g. under Xvfb) does.
- **Windows (off).** CEF 149's Windows sandbox comes with the bootstrap model
  (`USE_SANDBOX=ON` defines `CEF_USE_BOOTSTRAP`): the distribution's
  `bootstrap.exe` becomes the app's executable, creates the sandbox information
  (`cef_sandbox_info_create` is linked into it, not into `libcef.dll`), and
  calls `RunWinMain` in a client DLL named after the executable (`<app>.exe`
  loads `<app>.dll`). A signed bootstrap executable loads only a client DLL
  signed with the same certificate. laufey's host is a single `laufey.exe` today
  (`WinMain` in `main_windows.cc`, which also runs the headless worker mode),
  and a packaged app already uses `<app>.dll` for its runtime library (laufey
  loads the runtime named after the executable), so the bootstrap's client DLL
  and the runtime would need the same name. Turning it on therefore takes,
  together: the host built as a DLL exporting `RunWinMain` that passes the
  sandbox information to `CefExecuteProcess` and `CefInitialize`; the runtime
  renamed (on Windows only) in laufey's co-located runtime lookup and in Deno's
  packaging, installer and updater; the packaging shipping `bootstrap.exe` as
  `<app>.exe` with the app's icon and version resources; and signing both
  executables with one certificate. Until then, keep CEF windows on content the
  app serves itself (its custom schemes) and open other sites in the user's
  browser, as laufey's external-link handling does by default.

The native e2e battery checks the result from outside the backend
(`examples/native_e2e/src/sandbox_checks.rs`,
`scripts/native-e2e-run.sh
cef --sandbox`): on Linux every renderer has a
seccomp-bpf filter (`/proc/<pid>/status` `Seccomp: 2`), `NoNewPrivs: 1` and a
PID namespace of its own (a nested `NSpid`); on macOS `sandbox_check()` reports
the renderer and GPU helpers sandboxed; on Windows it reads the children's
integrity level. `LAUFEY_E2E_EXPECT_SANDBOX=0` asserts the opposite, for a Linux
run without a usable sandbox. On Linux `LAUFEY_E2E_EXPECT_SANDBOX_MODE` names
the layer (`namespace`: the renderer has a user namespace of its own; `setuid`:
it doesn't), checked against the host's `laufey: sandbox:` line too.

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
