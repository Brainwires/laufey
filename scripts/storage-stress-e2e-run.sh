#!/usr/bin/env bash
#
# localStorage across many quick relaunches: a chain of launches of one app
# (one LAUFEY_DATA_DIR), each of which reads what the launch before it stored,
# stores a fresh value, and quits at once. Every launch must read the previous
# launch's value: a quit must not lose the write it follows, and a launch must
# open the profile's storage on disk (not fall back to memory, which reads
# nothing and loses what the page stores). See docs/backends.md, "How an app
# ends".
#
#   scripts/storage-stress-e2e-run.sh <webview|cef> [launches]
#
# Each launch's page reads and stores as it loads, and the launch quits at
# once (examples/storage_e2e, LAUFEY_E2E_STORAGE_PREV).
# LAUFEY_E2E_STRESS_SCHEME=<name> serves the page over that custom scheme
# (declared with LAUFEY_CUSTOM_SCHEMES; CEF) instead of loopback HTTP, and
# LAUFEY_E2E_STRESS_ARGS adds switches to every launch (e.g.
# --enable-logging=stderr with LAUFEY_CEF_LOG_SEVERITY=info). On a failure
# the launch's log and, on CEF, the localStorage database's LevelDB info logs
# are printed.
#
# Build first: `cargo build --release -p storage_e2e_runtime` and the backend.
set -euo pipefail

backend="${1:?usage: storage-stress-e2e-run.sh <webview|cef> [launches]}"
launches="${2:-40}"
scheme="${LAUFEY_E2E_STRESS_SCHEME:-}"

rt=""
for c in \
  target/release/libstorage_e2e_runtime.so \
  target/release/libstorage_e2e_runtime.dylib \
  target/release/storage_e2e_runtime.dll; do
  if [ -f "$c" ]; then rt="$PWD/$c"; break; fi
done
[ -n "$rt" ] || { echo "storage_e2e_runtime cdylib not found (build it first)"; exit 1; }
export LAUFEY_RUNTIME_PATH="$rt"

case "$backend" in
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

case "$(uname -s)" in
  Darwin) platform=macos ;;
  Linux) platform=linux ;;
  *) platform=windows ;;
esac

if [ "$platform" = windows ]; then
  scratch="$(cygpath -u "${TEMP:-/tmp}")/laufey-stress-e2e"
else
  scratch="${TMPDIR:-/tmp}"
  scratch="${scratch%/}/laufey-stress-e2e"
fi
rm -rf "$scratch"
mkdir -p "$scratch/logs" "$scratch/data"
dir="$scratch/data/app"
dir_native="$dir"
[ "$platform" != windows ] || dir_native="$(cygpath -w "$dir")"
if [ "$platform" = linux ]; then
  export XDG_DATA_HOME="$scratch/xdg-data"
  mkdir -p "$XDG_DATA_HOME"
fi
cleanup() { [ -n "${KEEP_SCRATCH:-}" ] || rm -rf "$scratch" 2>/dev/null || true; }
trap cleanup EXIT

port=$((20000 + RANDOM % 20000))
timeout_s="${LAUFEY_E2E_EXIT_TIMEOUT:-60}"

# The localStorage database's LevelDB info logs (CEF): one open each.
ldb_dir() { find "$dir" -type d -path '*Local Storage/leveldb' 2>/dev/null | head -1 || true; }
show_ldb() { # <label>
  local d
  d="$(ldb_dir)"
  [ -n "$d" ] || return 0
  echo "    $1: $(cd "$d" && ls -l | tail -n +2 | awk '{print $5, $NF}' | tr '\n' ' ')"
  for f in LOG.old LOG; do
    [ -f "$d/$f" ] || continue
    echo "    $1 $f:"
    sed 's/^/      /' "$d/$f" | tail -20
  done
}

lost=0
other=0
hangs=0
prev="-"
start_all=$SECONDS
echo "== storage-stress-e2e: backend=$backend bin=$bin launches=$launches scheme=${scheme:-none}"
for ((i = 1; i <= launches; i++)); do
  value="v$RANDOM$RANDOM-$i"
  log="$scratch/logs/$i.log"
  # Keep the previous launch's database info logs: the next open rotates them.
  d="$(ldb_dir)"
  rm -rf "$scratch/ldb-before"
  [ -z "$d" ] || cp -R "$d" "$scratch/ldb-before" 2>/dev/null || true
  cmd=(env -u LAUFEY_APP_ID -u LAUFEY_E2E_STORAGE_EXPECT -u LAUFEY_E2E_STORAGE_HOLD_MS
    -u LAUFEY_CUSTOM_SCHEMES -u LAUFEY_E2E_STORAGE_SCHEME
    LAUFEY_DATA_DIR="$dir_native" LAUFEY_E2E_STORAGE_PORT="$port"
    LAUFEY_E2E_STORAGE_MODE=write LAUFEY_E2E_STORAGE_VALUE="$value"
    LAUFEY_E2E_STORAGE_PREV="$prev" LAUFEY_E2E_STORAGE_EXIT=quit)
  if [ -n "$scheme" ]; then
    cmd+=(LAUFEY_CUSTOM_SCHEMES="$scheme" LAUFEY_E2E_STORAGE_SCHEME="$scheme")
  fi
  if [ "$platform" = linux ]; then
    cmd+=(xvfb-run -a dbus-run-session -- "$bin")
  else
    cmd+=("$bin")
  fi
  # Extra engine switches while debugging (e.g. --enable-logging=stderr).
  # shellcheck disable=SC2206
  [ -z "${LAUFEY_E2E_STRESS_ARGS:-}" ] || cmd+=(${LAUFEY_E2E_STRESS_ARGS})
  "${cmd[@]}" >"$log" 2>&1 &
  pid=$!
  waited=0
  while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt "$timeout_s" ]; do
    sleep 1
    waited=$((waited + 1))
  done
  rc=0
  if kill -0 "$pid" 2>/dev/null; then
    rc=124
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  else
    wait "$pid" || rc=$?
  fi
  # The launch's one report line: what its page read before its write, and
  # what it read back after it.
  report="$(grep -m1 '^\[e2e\] previous localStorage=' "$log" || true)"
  got="$(printf '%s' "$report" | sed -n 's/^\[e2e\] previous localStorage="\([^"]*\)".*/\1/p')"
  back="$(printf '%s' "$report" | sed -n 's/.* back="\([^"]*\)".*/\1/p')"
  want="$prev"
  [ "$want" != "-" ] || want="null"
  if [ "$rc" = 124 ]; then
    hangs=$((hangs + 1))
    echo "[stress-e2e] FAIL #$i: the launch didn't end within ${timeout_s}s"
    sed 's/^/    | /' "$log" | tail -40
    break
  fi
  if [ -n "$report" ] && [ "$got" = "$want" ] && [ "$back" = "$value" ] &&
    [ "$rc" = 0 ]; then
    prev="$value"
    continue
  fi
  if [ -n "$report" ] && [ "$got" != "$want" ]; then
    lost=$((lost + 1))
    echo "[stress-e2e] FAIL #$i: read \"$got\", want \"$want\" (exit $rc)"
  else
    other=$((other + 1))
    echo "[stress-e2e] FAIL #$i: the launch (exit $rc, report '${report}')"
  fi
  sed 's/^/    | /' "$log" | grep -v '^    | \[e2e\] PASS' | tail -60
  if [ -d "$scratch/ldb-before" ]; then
    d_saved="$(ldb_dir)"
    echo "    the database before this launch:"
    for f in LOG.old LOG; do
      [ -f "$scratch/ldb-before/$f" ] || continue
      echo "    before $f:"
      sed 's/^/      /' "$scratch/ldb-before/$f" | tail -20
    done
    [ -z "$d_saved" ] || show_ldb "after"
  fi
  # Go on from what this launch stored, if it read back its own write.
  [ "$back" != "$value" ] || prev="$value"
done

echo "[stress-e2e] $launches launches in $((SECONDS - start_all)) s: $lost didn't" \
  "read the previous launch's value, $other failed otherwise, $hangs hung"
if [ "$lost" = 0 ] && [ "$other" = 0 ] && [ "$hangs" = 0 ]; then
  echo "[stress-e2e] OVERALL PASS"
else
  echo "[stress-e2e] OVERALL FAIL"
  exit 1
fi
