# B0 spike: GPUI and a deno_core isolate in one process

Date: 2026-11-23. Branch `spike/gpui-b0`. Plan: thalora-web-browser `docs/GPUI_PLAN.md`, phase B0.

## Verdict: GO for B1, with conditions

On macOS arm64, one native binary runs a `gpui-pre` window on the main thread and a deno_core
`JsRuntime` from the Brainwires/deno fork on a second thread, linked against the fork's
prebuilt rusty_v8 with custom libc++. JS commits a UI tree through an op, GPUI draws it, a
press goes back to JS, JS commits again, and GPUI redraws. It linked and ran on the first
attempt, with no link-level workaround.

Conditions (details below):

1. **Windows and Linux are unproven.** Only macOS arm64 was built. The link risk is highest on
   Windows (static CRT). The CI job `build-gpui` (see "CI job") builds and self-tests macOS
   arm64, Windows x64 and Linux x64; B1 should not depend on a target before it passes there.
2. **Do not consume deno_core as a git dependency on the whole fork** beyond the spike. It
   works, but cargo checks out every submodule of the fork (WPT included): 3.5 minutes and
   2.3 GB for the first resolve. See "Proposed DENEXT.md items".
3. **Packaging is unverified:** the kernel-sandbox profile and macOS signing/notarization.
4. **Clippy and the multi-target checks are deferred to the train** (user policy, 2026-11-23).

## What was built

`laufey-gpui-spike`, its own cargo workspace in `backends/gpui/` (not a member of laufey's root
workspace; the root `Cargo.toml` and `Cargo.lock` are untouched, and cargo needed no `exclude`
line).

| File | Role |
| --- | --- |
| `src/main.rs` | Arguments, V8 platform init on the main thread, starts both sides |
| `src/js.rs` | The "js" thread, the `laufey_gpui` extension and its two ops |
| `src/app.js` | The embedded app: a title and a button that counts presses |
| `src/tree.rs` | The JSON tree and event types |
| `src/ui.rs` | The GPUI view, the tree-to-`div()` mapping and the self-test |
| `build.sh` | Verifies or fetches the prebuilt V8, sets the env, runs cargo |
| `rusty_v8.sha256` | Copy of the fork's manifest of V8 archive hashes |

Ops:

- `op_gpui_commit(json)`: sync, `#[op2(fast)]`. Takes the whole tree as a JSON string and
  replaces the previous one. Node: `{id, type: "view"|"text"|"pressable", style, text?,
  children}`. Style subset: `flex`, `flexDirection`, `alignItems`, `justifyContent`, `padding`,
  `gap`, `width`, `height`, `backgroundColor`, `color`, `fontSize`, `borderRadius`.
- `op_gpui_next_event()`: async. Resolves with `{"type":"press","id":N}` as a JSON string, or
  `null` once the window has closed the event channel.

## Threading model

- **Main thread:** GPUI's `App` (`gpui_platform::application().run`).
- **"js" thread:** a current-thread tokio runtime that owns the `JsRuntime` for its whole life.
- **JS to UI:** a `futures` unbounded channel. A GPUI foreground task (`cx.spawn`) awaits it and
  calls `cx.notify()` on the view. Nothing polls. (ParolNet's video view polls a slot every
  8 ms; a channel was simpler here and gives a true round-trip figure.)
- **UI to JS:** a tokio unbounded channel. `on_click` sends from the main thread, which wakes
  the pending `op_gpui_next_event` on the js thread.
- **Shutdown:** the UI drops the event sender, the op resolves `null`, the app's loop ends, the
  event loop drains, and the isolate is dropped on its own thread. The UI joins the thread and
  then exits.

Nothing fought this model. Three points to carry into B1:

- `JsRuntime::init_platform(None)` is called on the main thread before the js thread starts.
  V8's platform is process-wide; on Linux its memory-protection keys must be set up on the
  thread that later threads are created from. Not exercised on macOS.
- The isolate never touches the main thread, so no pump is needed. The cost is that every
  commit crosses a thread (0.25 ms here, including JS and JSON).
- Synchronous JS-to-UI calls (measuring layout, for example) cannot be answered from this model
  without blocking the js thread on the main thread. B1 should keep the protocol one-way plus
  events, as planned.

## Did it link?

Yes. Findings about the custom libc++ next to GPUI:

- The binary has 9,654 `std::__Cr` symbols (V8's own libc++, statically linked) and **no
  `std::__1` symbols at all, defined or undefined**. `otool -L` shows no `libc++.1.dylib`.
- On macOS, GPUI's dependency tree in this configuration brings no C++: its native side is
  Objective-C frameworks (AppKit, Metal, CoreText, QuartzCore, and others) and C. So there is
  only one C++ standard library in the process, and the two-libc++ conflict the plan worried
  about does not arise on macOS.
- No duplicate-symbol errors or linker warnings. No ObjC runtime issues: `libobjc` is linked
  once, by GPUI; V8 does not use it.
- This result does not transfer to Linux (system libstdc++ may come in through GPUI's
  dependencies) or to Windows (see "Unverified").

## Build problems hit and how they were resolved

| Problem | Resolution |
| --- | --- |
| Xcode's `metal` compiler is not installed on this machine (Command Line Tools only), so GPUI's build-time shader compile cannot run | `runtime-shaders` feature (ParolNet's pattern); `build.sh` adds it when `xcrun -f metal` fails. Shaders are compiled at launch. |
| The fork's root `Cargo.toml` gives deno_core `default-features = false` with `reactor-tokio` and `v8_use_custom_libcxx`, which selects no JS engine (the fork lets each binary choose V8 or QuickJS) | Added the `v8` feature. |
| A git dependency does not carry the fork's `[patch.crates-io]` | Repeated the entries in deno_core's graph: `v8` (required), and `url` 2.5.7 and `sys_traits` 0.1.28 from the fork's vendored copies, so deno_core compiles against the same sources as in the fork. The lockfile confirms all three. |
| The first resolve of the git dependency clones the fork's submodules (WPT, node_test, deno_std, lsp benchdata, lzld) | Accepted for the spike: 3 min 30 s, 2.3 GB in `~/.cargo/git/checkouts`. Not acceptable for CI; see the proposed DENEXT.md item. |
| `~/.cache/denext-rusty-v8/env.txt` names a stale hash (`8f7387…`) | Ignored. The archive and binding in that directory do match the pinned manifest (hashes below). The copy in `/Volumes/Workbench/denext-fork-target/rusty_v8/` does not match and was not used. |
| One borrow-check error in the spike's own code | Fixed. Not a toolchain or dependency issue. |

No fork change was needed.

## Pins and environment

| | |
| --- | --- |
| Toolchain | Rust 1.99.1 (`rust-toolchain.toml`), the same as laufey and the fork |
| GPUI | `gpui-pre =0.3.12`, `gpui-pre-platform =0.3.12` with `font-kit`, `x11`, `wayland` |
| deno_core | 0.413.0, `https://github.com/Brainwires/deno` rev `65ff17c86f75a3d4be53bc6100c77a880cd2918f` (tip of `train/denext30` on 2026-11-23), features `v8`, `reactor-tokio`, `v8_use_custom_libcxx` |
| v8 | `156.0.0+denext.6`, `https://github.com/Brainwires/rusty_v8` rev `f72a5f53280688d3101ab6f82e4f7b5ccf528ecd`: the rev the pinned deno rev's `[patch.crates-io]` and `Cargo.lock` name (verified) |
| V8 archive | Release `v156.0.0-denext.6`, `librusty_v8_release_aarch64-apple-darwin.a.gz`, SHA-256 `62777aade1446cd5add5b3b1d622e3c8140cb42b078e9bb7a18a614449b45fa3` |
| V8 binding | `src_binding_release_aarch64-apple-darwin.rs`, SHA-256 `4111dcffa08d6b7484bca2665264b65d16b6e8a220091e967ab86452e72d53ee` |
| deno_error | `=0.7.1` (the fork's pin) |

Environment, set by `build.sh`:

```sh
RUSTY_V8_ARCHIVE=$HOME/.cache/denext-rusty-v8/librusty_v8_release_aarch64-apple-darwin.a.gz
RUSTY_V8_ARCHIVE_SHA256=62777aade1446cd5add5b3b1d622e3c8140cb42b078e9bb7a18a614449b45fa3
RUSTY_V8_SRC_BINDING_PATH=$HOME/.cache/denext-rusty-v8/src_binding_release_aarch64-apple-darwin.rs
```

`.cargo/config.toml` repeats the fork's guard (`RUSTY_V8_MIRROR` set to a refusing value,
`RUSTY_V8_MIRROR_FALLBACK=0`), so a build without a verified archive fails instead of
downloading one. V8 was never built from source.

Reproduce:

```sh
backends/gpui/build.sh                                   # cargo build --release
backends/gpui/target/release/laufey-gpui-spike --self-test
```

Not copied from the fork: its release profile (fat LTO, `opt-level = "z"`, `panic = "abort"`),
its `-fuse-ld=lld -Wl,--icf=safe -Wl,-fixup_chains` link arguments, `--cfg tokio_unstable`
(nothing in deno_core's graph reads it) and `MACOSX_DEPLOYMENT_TARGET=12.0` (the spike binary
has `minos 11.0`). `gpui-component` was left out: the host maps React Native primitives onto raw
GPUI elements and uses none of its widgets, so there is no `Root` wrapper.

## Measurements

Machine: Apple M5 Pro, 18 cores, 24 GB, macOS 27.2.1. Other sessions were building at the same
time (load average about 5), so treat the times as upper bounds.

| | |
| --- | --- |
| First dependency resolve (git clones with submodules) | 3 min 30 s |
| Clean release build | about 1 min 38 s: 1 min 32 s wall (400 s CPU) for all 381 dependency crates, plus about 6 s for the spike crate and the link. Measured in two parts because the first build stopped at the borrow-check error in the last crate. V8 is not compiled; its build script unpacks the archive. |
| Rebuild after editing the spike's sources | about 6 s (5.6 s and 5.9 s seen while fixing the spike; not measured as a separate exercise) |
| Release binary, raw | 72,838,608 bytes (69.5 MiB) |
| Release binary, stripped (`strip`) | 53,681,120 bytes (51.2 MiB) |
| `target/` after one release build | 1.5 GB |
| Lockfile | 724 packages |

Self-test, one run, 20 presses:

```
b0-window first-frame ms=1814
b0-result presses=20 counter=20 first_commit_ms=1739.0 first_frame_ms=1815.0 round_trip_ms_avg=33.171 round_trip_commit_ms_avg=0.249
```

- `first_commit_ms`: launch to the first tree arriving in the view. `first_frame_ms`: launch to
  the start of the frame after the one that drew it, so the tree has been on screen by then.
- `round_trip_commit_ms_avg`: press sent to the recommitted tree arriving in the view. This is
  the cost of the two thread hops, the op, JS and JSON both ways: **0.25 ms**.
- `round_trip_ms_avg`: press sent to the start of the frame after the redraw: **33 ms**, which
  is two 60 Hz display refreshes (one to draw, one for the next-frame callback that stops the
  clock). The pipeline's own latency to a drawn frame is at most one refresh.
- The 1.8 s to first frame was not broken down (the self-test was run once, per the load
  policy). JS is not the cause: the app commits as soon as the isolate starts and the tree then
  waits for the window. The likely costs are the launch-time Metal shader compile from
  `runtime-shaders`, and opening the window on a first launch. Measure it in B1 with
  build-time shaders.

## Not done here, deferred to the train

- **Clippy: deferred to train.** `cargo clippy -- -D warnings` was not run locally. The release
  build has no rustc warnings and `cargo fmt` was applied.
- The self-test ran once and passed. No other tests exist.
- No separate incremental-rebuild measurement (the figure above is incidental).

### CI job

`build-gpui` in laufey's `.github/workflows/ci.yml` ("gpui spike / <target>") now does this. It
needs the `runners` job like `build-winit` and `build-webview`, and its matrix follows theirs:

| Target | Runner | When |
| --- | --- | --- |
| `x86_64-unknown-linux-gnu` | `ubuntu-22.04` | every run |
| `x86_64-pc-windows-msvc` | `windows-latest` | full runs |
| `aarch64-apple-darwin` | pick key `gpui_macos_arm64` (our macOS arm64 pool, else `macos-15`) | full runs |
| `x86_64-apple-darwin` | `macos-15-intel` | full runs dispatched with `macos_runner=macos-15-intel`, instead of arm64 |

Each leg:

1. Installs the toolchain from `rust-toolchain.toml` (1.99.1 with clippy and rustfmt).
2. Linux: installs GPUI's system packages (ParolNet's `desktop-spike.yml` list less its media
   packages: xkbcommon, wayland, xcb, fontconfig, Vulkan with Mesa's lavapipe, Xvfb). macOS:
   `build.sh` adds `runtime-shaders` when the runner's Xcode has no `metal`.
3. Caches cargo and `backends/gpui/target` with `Swatinem/rust-cache`, keyed on this
   workspace's `Cargo.lock` and the runner image. `~/.cargo/git/checkouts` is not cached, so
   every run still checks the fork out with its submodules from the cached git database.
4. `build.sh fetch --seed target/release`: downloads the target's V8 library
   (`librusty_v8_release_<target>.a.gz`, `rusty_v8_release_<target>.lib.gz` on Windows) and
   binding from the release `rusty_v8.sha256` names, checks both hashes and that the release tag
   matches `Cargo.lock`'s `v8` version, and unpacks the library to `gn_out/obj`, as the fork's
   `fetch_rusty_v8.py --seed-dir` does. `build.sh` runs under Git Bash on Windows.
5. `build.sh clippy --release --locked -- -D warnings`.
6. `build.sh build --release --locked`.
7. `laufey-gpui-spike --self-test` (under `xvfb-run` on Linux, on the runner's desktop
   elsewhere). The leg fails unless the exit code is 0 and stdout has a
   `b0-result presses=20 counter=20` line. Stdout, stderr, the exit code and the binary's size in
   bytes are uploaded as the artifact `gpui-spike-<target>`.

Not in the job yet: `cargo fmt --check` for this workspace (the `lint` job runs it for the root
workspace only), aarch64 Linux and Windows, a check that the root `Cargo.lock` is unchanged,
and the job does not gate `release`.

## Unverified, to check later

**Windows**
- The fork builds with `-C target-feature=+crt-static` and V8's custom libc++. GPUI's Windows
  dependencies (DirectX, DirectWrite, and any C code built by `cc`) must then also link the
  static CRT. A CRT mismatch (`/MT` against `/MD`) is the most likely link failure of the whole
  plan. This workspace's `.cargo/config.toml` now sets the fork's rustflags for
  `x86_64-pc-windows-msvc` (`+crt-static` and `symbol-mangling-version=v0`). **Unverified until
  the `build-gpui` CI job runs on Windows.** `aarch64-pc-windows-msvc` has no entry yet.
- The fork also sets `/STACK:4194304` for the main thread, where its isolate runs. Here the
  isolate is on the "js" thread, which `src/js.rs` now spawns with a 4 MiB stack
  (`std::thread::Builder::stack_size`) instead of Rust's default 2 MiB; the main thread keeps
  the linker's default. **Unverified until the CI job runs on Windows.**

**Linux**
- GPUI renders through wgpu and links system libraries (xkbcommon, wayland, x11, fontconfig).
  Whether any of them brings libstdc++ into the process next to V8's `std::__Cr` was not
  checked. Deno itself coexists with libstdc++ on Linux, so this is expected to work.
- The `init_platform` on the main thread requirement only matters there.

**Kernel-sandbox desktop profile (5.0)**
- Not run. What this binary needs at launch, from reading the code and `otool -L`: the system
  frameworks listed by `otool -L` (AppKit, Metal, CoreText, QuartzCore, UserNotifications,
  Security, and others), read access to system fonts for CoreText (`font-kit` feature), a
  window-server connection, and Metal. With `runtime-shaders`, Metal compiles shader source in
  process at launch; with build-time shaders the compiled library is embedded in the binary.
  Either way Metal keeps its own pipeline cache in the per-user cache directory, which the
  profile must allow writing or Metal must tolerate being denied. This is inference, not a
  test.
- V8 needs executable memory (JIT), as the fork's existing binaries do.

**macOS signing and notarization**
- Not attempted. Expected needs: the hardened runtime with `com.apple.security.cs.allow-jit`
  for V8 (the same entitlement the fork's deno binaries need). GPUI loads no dylibs of its own
  and embeds no fonts in this configuration, so there is nothing extra to sign inside the
  bundle. `runtime-shaders` should not be shipped: it costs launch time, and build-time shaders
  need the Metal toolchain on the packaging machine.
- GPUI links `UserNotifications.framework`; whether that needs a bundle identifier at launch
  inside a signed app was not checked. The unbundled binary runs.

**Other**
- Licences: `cargo deny` was not run. ParolNet's MPL-2.0 exceptions (`cbindgen`, `dwrote`,
  `option-ext`) apply to this tree too, and deno_core's graph adds its own crates to review.
- The self-test needs a window (GPUI's headless macOS platform has none). It ran on a machine
  with a display; CI needs the runner desktop or Xvfb.

## Proposed DENEXT.md items (fork changes, not made here)

1. **A way to consume deno_core without the whole fork checkout.** Options: publish
   `deno_core`, `deno_ops`, `serde_v8`, `deno_v8` and the vendored `url` / `sys_traits` to a
   Brainwires registry; or a slim mirror repository of `libs/` generated per train; or have the
   host built inside the fork's workspace. Today a git dependency costs 3.5 minutes and 2.3 GB
   on a cold cache, because cargo checks out all submodules.
2. **Make the fork's `[patch.crates-io]` entries that deno_core needs discoverable.** A consumer
   has to repeat `v8`, `url` and `sys_traits` by hand, and nothing fails if one is forgotten: the
   build silently uses the crates.io `url` and `sys_traits`.
3. **`fetch_rusty_v8.py` usable from outside the fork.** It reads the fork's own `Cargo.lock`
   and manifest by relative path, so this workspace carries a copy of `rusty_v8.sha256` and a
   shell reimplementation of the check in `build.sh`. A `--manifest` / `--lock` argument would
   let laufey call the script as is.

## For B1

- The JSON tree is replaced by mutation batches in a shared buffer; the two-channel shape and
  the `cx.spawn` consumer can stay.
- The view rebuilds every element on each commit. B1's retained node tree should notify only
  what changed.
- Decide the host's home after the CI job has run on all targets: the standalone workspace
  worked well and kept GPUI's 724-package lockfile out of laufey's root.
