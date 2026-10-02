#!/usr/bin/env bash
#
# Run the backend-agnostic native_e2e_runtime under a given backend and
# propagate its PASS/FAIL exit code. See docs/e2e-testing.md.
#
#   scripts/native-e2e-run.sh <winit|webview|cef> [--layer1|--scheme-body|--lifetime|--window-api|--io|--system|--devtools-off]
#
# --layer1 (Linux only) wraps the run in the D-Bus StatusNotifier/dbusmenu
# observer (native_e2e_driver) under a private session bus.
# --scheme-body runs only the custom-scheme request-body round trip (for
# backends where the full battery can't run in CI).
# --lifetime runs only the app-lifetime checks (keep-alive with no window,
# then quit() ending the event loop); they end the process, so they can't
# share the main battery's run.
# --window-api runs only the API 38 window checks (state, constraints,
# screens, title bar, backdrops); handy under a real window manager.
# --io runs only the API 39 checks: drag and drop (through the test hook),
# real file dialogs driven by the test hook, the rich clipboard.
# --system runs only the API 40 checks: global shortcuts (including a
# conflict with a second process), launch at login (only in CI or with
# LAUFEY_E2E_LOGIN_ITEM=1) and DevTools open / close / toggle.
# --devtools-off runs the DevTools checks under LAUFEY_INSPECTABLE=0.
# --menus-notifications runs only the API 41 checks: menu accelerators, the
# context-menu close callback, notification responses, live callbacks and
# scheduling. On Linux it starts a stand-in notification server
# (laufey_mock_notification_server) on the run's private session bus.
set -euo pipefail

backend="${1:?usage: native-e2e-run.sh <winit|webview|cef> [--layer1|--scheme-body|--lifetime|--window-api|--io|--system|--devtools-off|--menus-notifications]}"
mode="${2:-}"

# Locate the runtime cdylib (.so / .dylib / .dll).
rt=""
for c in \
  target/release/libnative_e2e_runtime.so \
  target/release/libnative_e2e_runtime.dylib \
  target/release/native_e2e_runtime.dll; do
  if [ -f "$c" ]; then rt="$PWD/$c"; break; fi
done
[ -n "$rt" ] || { echo "native_e2e_runtime cdylib not found (build it first)"; exit 1; }
export LAUFEY_RUNTIME_PATH="$rt"

# The battery serves a page over its own custom scheme (laufey-e2e://app/)
# and asserts it is a real origin. WebView backends learn the scheme from the
# runtime's register_scheme_handler call, but CEF registers custom schemes at
# process start (before the runtime is loaded) and must be told up front —
# see custom_schemes.h. Harmless for the other backends.
export LAUFEY_CUSTOM_SCHEMES=laufey-e2e
# Lets the battery tell backends that share a target OS apart (the late
# scheme registration check differs between WebKitGTK and CEF on Linux).
export LAUFEY_E2E_BACKEND="$backend"
if [ "$mode" = "--scheme-body" ]; then
  export LAUFEY_E2E_ONLY=scheme-body
fi
if [ "$mode" = "--lifetime" ]; then
  export LAUFEY_E2E_ONLY=lifetime
fi
if [ "$mode" = "--window-api" ]; then
  export LAUFEY_E2E_ONLY=window-api
fi
if [ "$mode" = "--io" ]; then
  export LAUFEY_E2E_ONLY=io
fi
if [ "$mode" = "--system" ]; then
  export LAUFEY_E2E_ONLY=system
  # Names the login entry (HKCU Run value, XDG autostart file).
  export LAUFEY_APP_ID="${LAUFEY_APP_ID:-dev.laufey.e2e.system}"
fi
if [ "$mode" = "--devtools-off" ]; then
  export LAUFEY_E2E_ONLY=devtools-off
  export LAUFEY_INSPECTABLE=0
fi
mock=""
if [ "$mode" = "--menus-notifications" ]; then
  export LAUFEY_E2E_ONLY=menus-notifications
  # Names the Windows AppUserModelID registration and the Linux
  # desktop-entry hint; the schedule file lives in this data directory.
  export LAUFEY_APP_ID="${LAUFEY_APP_ID:-dev.laufey.e2e.notifications}"
  if [ "$(uname -s)" = "Linux" ]; then
    export LAUFEY_DATA_DIR="$(mktemp -d "${TMPDIR:-/tmp}/laufey-e2e-data.XXXXXX")"
    mock="$(ls webview/build/backend-common/laufey_mock_notification_server \
      cef/build/backend-common/laufey_mock_notification_server \
      cef/build/laufey_mock_notification_server 2>/dev/null | head -1 || true)"
    if [ -n "$mock" ]; then
      export LAUFEY_E2E_NOTIFY_MOCK="$PWD/$mock"
    fi
  fi
fi

# Resolve the backend binary (handles macOS .app bundles).
case "$backend" in
  winit)
    bin="$(ls target/release/laufey_winit target/release/laufey_winit.exe 2>/dev/null | head -1 || true)" ;;
  webview)
    bin="$(ls \
      webview/build/laufey_webview.app/Contents/MacOS/laufey_webview \
      webview/build/laufey_webview \
      webview/build/laufey_webview.exe 2>/dev/null | head -1 || true)" ;;
  cef)
    bin="$(ls \
      cef/build/Release/laufey.app/Contents/MacOS/laufey \
      cef/build/Release/laufey \
      cef/build/Release/laufey.exe 2>/dev/null | head -1 || true)" ;;
  *) echo "unknown backend: $backend"; exit 2 ;;
esac
[ -n "$bin" ] || { echo "backend binary for '$backend' not found (build it first)"; exit 1; }

# The custom-scheme check fetches a loopback echo server from the
# laufey-e2e:// page. Chromium's Local Network Access checks treat that as a
# public origin reaching the local network and hold the request for a
# permission prompt the CEF host never shows, so the fetch would hang; the
# check is about the Origin header, not LNA, so switch LNA off for the run.
args=()
if [ "$backend" = "cef" ]; then
  args+=(--disable-features=LocalNetworkAccessChecks)
fi
echo "== native-e2e: backend=$backend bin=$bin runtime=$rt =="

is_linux() { [ "$(uname -s)" = "Linux" ]; }

if [ "$mode" = "--layer1" ]; then
  is_linux || { echo "--layer1 is Linux-only"; exit 2; }
  export LAUFEY_E2E_HOLD=1
  driver="$(ls target/release/native_e2e_driver 2>/dev/null | head -1 || true)"
  [ -n "$driver" ] || { echo "native_e2e_driver not built"; exit 1; }
  exec xvfb-run -a dbus-run-session -- "$driver" "$bin" ${args[@]+"${args[@]}"}
fi

# Layer 0: stream the output (so a hang shows how far the battery got) and
# keep a copy, so an unexpected native termination cannot masquerade as
# success merely because macOS reports an exit code of 0. On Linux, run
# headless via Xvfb + a private session bus (some tray implementations need
# it).
#
# A watchdog bounds the run (LAUFEY_E2E_WATCHDOG_SECS, default 300; the whole
# battery takes well under a minute). When it fires it prints every thread's
# stack (macOS `sample`, Linux gdb when installed) before killing the
# process, so a hang leaves evidence instead of only a step timeout.
log="$(mktemp "${TMPDIR:-/tmp}/native-e2e.XXXXXX")"
run_backend() {
  if is_linux && [ -n "$mock" ]; then
    # The stand-in notification server owns the name on the same private
    # session bus before the backend starts.
    exec xvfb-run -a dbus-run-session -- sh -c \
      '"$LAUFEY_E2E_NOTIFY_MOCK" & for _ in 1 2 3 4 5 6 7 8 9 10; do sleep 0.2; done; exec "$@"' \
      sh "$bin" ${args[@]+"${args[@]}"}
  elif is_linux; then
    exec xvfb-run -a dbus-run-session -- "$bin" ${args[@]+"${args[@]}"}
  else
    exec "$bin" ${args[@]+"${args[@]}"}
  fi
}
run_backend > >(tee "$log") 2>&1 &
pid=$!
watchdog_secs="${LAUFEY_E2E_WATCHDOG_SECS:-300}"
(
  waited=0
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$waited" -ge "$watchdog_secs" ]; then
      echo "native e2e: watchdog: no exit after ${watchdog_secs}s; stacks follow" >&2
      case "$(uname -s)" in
        Darwin)
          # The backend and its helper processes (CEF renderer / GPU).
          for p in "$pid" $(pgrep -P "$pid" 2>/dev/null || true); do
            sample "$p" 3 -mayDie 2>&1 | head -c 200000 >&2 || true
          done ;;
        Linux)
          if command -v gdb >/dev/null; then
            for p in $(pgrep -f "$bin" 2>/dev/null || true); do
              gdb -p "$p" -batch -ex "thread apply all bt" 2>&1 |
                head -c 200000 >&2 || true
            done
          fi ;;
      esac
      kill -9 "$pid" 2>/dev/null || true
      break
    fi
    sleep 1
    waited=$((waited + 1))
  done
) &
watchdog=$!
set +e
wait "$pid"
status=$?
set -e
wait "$watchdog" 2>/dev/null || true
# Let tee drain what the process wrote last.
sleep 1
echo "== native-e2e: backend exited with status $status =="
if ! grep -q '^\[e2e\] OVERALL ' "$log"; then
  echo "native e2e exited before reporting an overall result" >&2
  rm -f "$log"
  exit 1
fi
rm -f "$log"
exit "$status"
