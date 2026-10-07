#!/usr/bin/env bash
#
# The Linux CEF host ends cleanly on SIGTERM, SIGINT and SIGHUP: through the
# app's own quit (windows closed, runtime shut down, CefShutdown), not
# Chromium's session-end path, which _exit()s the browser process and leaves
# the profile marked "SessionEnded" and the child processes orphaned.
#
#   scripts/signal-exit-e2e-run.sh [SIGTERM SIGINT SIGHUP]
#
# Runs the hello runtime (make cef hello-runtime) under Xvfb and a private
# session bus. For each signal: the host exits with status 0 (not killed by
# a signal: 143 / 130 / 129, nor 133 for a SIGTRAP crash), every process it
# started is gone, the profile's exit_type is "Normal", and no core file or
# apport report appeared.
set -euo pipefail

[ "$(uname -s)" = Linux ] || { echo "Linux only"; exit 2; }
bin="$PWD/cef/build/Release/laufey"
rt="$PWD/target/release/libhello_runtime.so"
[ -x "$bin" ] || { echo "$bin not built (make cef)"; exit 1; }
[ -f "$rt" ] || { echo "$rt not built (make hello-runtime)"; exit 1; }
signals=("$@")
[ "${#signals[@]}" -gt 0 ] || signals=(SIGTERM SIGINT SIGHUP)

fail=0
check() { # <ok?> <message>
  if [ "$1" = 1 ]; then echo "[e2e] PASS $2"; else echo "[e2e] FAIL $2"; fail=1; fi
}

for sig in "${signals[@]}"; do
  work="$(mktemp -d "${TMPDIR:-/tmp}/laufey-signal-e2e.XXXXXX")"
  crash_before="$(ls /var/crash 2>/dev/null | grep -c laufey || true)"
  # The host is the shell's direct child, so the shell sees its real status.
  (
    cd "$work"
    ulimit -c unlimited || true
    LAUFEY_DATA_DIR="$work/data" E2E_WORK="$work" \
      xvfb-run -a dbus-run-session -- sh -c '
      "$@" &
      echo $! >"$E2E_WORK/host.pid"
      wait $!
      echo $? >"$E2E_WORK/host.status"' sh \
      "$bin" --runtime "$rt" --password-store=basic >"$work/host.log" 2>&1
  ) &
  runner=$!
  # Up: the profile is written (the page loads within the next seconds).
  up=0
  for _ in $(seq 1 300); do
    if [ -s "$work/host.pid" ] && [ -f "$work/data/CEF/Default/Preferences" ]; then
      up=1
      break
    fi
    sleep 0.1
  done
  host="$(cat "$work/host.pid" 2>/dev/null || true)"
  check "$up" "$sig: the app came up (host $host)"
  sleep 3
  kill "-${sig#SIG}" "$host" 2>/dev/null || true
  for _ in $(seq 1 300); do
    [ -f "$work/host.status" ] && break
    sleep 0.1
  done
  status="$(cat "$work/host.status" 2>/dev/null || echo none)"
  wait "$runner" 2>/dev/null || true
  sleep 1
  check "$([ "$status" = 0 ] && echo 1)" "$sig: the host exits with status 0 (got $status)"
  left="$(pgrep -f "^$bin" | tr '\n' ' ' || true)"
  check "$([ -z "$left" ] && echo 1)" "$sig: no process of the app is left (${left:-none})"
  exit_type="$(grep -o '"exit_type":"[A-Za-z]*"' "$work/data/CEF/Default/Preferences" 2>/dev/null || true)"
  check "$([ "$exit_type" = '"exit_type":"Normal"' ] && echo 1)" \
    "$sig: the profile was shut down normally (${exit_type:-no exit_type})"
  cores="$(find "$work" -maxdepth 2 -name 'core*' | tr '\n' ' ')"
  crash_after="$(ls /var/crash 2>/dev/null | grep -c laufey || true)"
  check "$([ -z "$cores" ] && [ "$crash_after" = "$crash_before" ] && echo 1)" \
    "$sig: no core file or crash report (${cores:-no core}, /var/crash: $crash_before -> $crash_after)"
  if [ -n "$left" ]; then kill -9 $left 2>/dev/null || true; fi
  [ "$fail" = 0 ] || { echo "== host log ($sig) =="; tail -40 "$work/host.log"; }
  rm -rf "$work"
done
exit "$fail"
