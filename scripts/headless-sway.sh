#!/usr/bin/env bash
# Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
#
# Runs a command against a private headless sway (a wlroots compositor with
# no outputs or input devices, no Xwayland), with WAYLAND_DISPLAY pointing at
# it, SWAYSOCK at its IPC socket (for swaymsg) and DISPLAY unset, and stops
# sway afterwards. Exits with the command's status. HEADLESS_SWAY_FLOATING=1
# floats every window, so a window keeps the size it asks for (a tiled one
# takes its tile's). For tests that need a real compositor with ext-data-control-v1
# (sway 1.11+, wlroots 0.19+), in CI or locally:
#
#   scripts/headless-sway.sh build/laufey_clipboard_wayland_test
set -euo pipefail

if [ "$#" -eq 0 ]; then
  echo "usage: $0 <command> [args...]" >&2
  exit 2
fi

own_runtime=""
if [ -z "${XDG_RUNTIME_DIR:-}" ]; then
  XDG_RUNTIME_DIR="$(mktemp -d)"
  own_runtime="$XDG_RUNTIME_DIR"
  chmod 700 "$XDG_RUNTIME_DIR"
fi
export XDG_RUNTIME_DIR
dir="$(mktemp -d)"
printf 'xwayland disable\n' >"$dir/config"
if [ -n "${HEADLESS_SWAY_FLOATING:-}" ]; then
  printf 'for_window [shell="xdg_shell"] floating enable\ndefault_floating_border none\n' \
    >>"$dir/config"
fi
socket=""

env -u DISPLAY -u WAYLAND_DISPLAY \
  WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
  sway -c "$dir/config" -d >"$dir/sway.log" 2>&1 &
sway_pid=$!
cleanup() {
  kill "$sway_pid" 2>/dev/null || true
  wait "$sway_pid" 2>/dev/null || true
  rm -rf "$dir"
  if [ -n "$own_runtime" ]; then rm -rf "$own_runtime"; fi
}
trap cleanup EXIT

# sway names its socket itself (wayland-N): wait for the one it announces.
for _ in $(seq 1 100); do
  socket="$(grep -oE 'Running compositor on wayland display .wayland-[0-9]+' \
    "$dir/sway.log" 2>/dev/null | grep -oE 'wayland-[0-9]+' | head -1 || true)"
  [ -n "$socket" ] && [ -S "$XDG_RUNTIME_DIR/$socket" ] && break
  if ! kill -0 "$sway_pid" 2>/dev/null; then
    echo "headless-sway: sway exited" >&2
    cat "$dir/sway.log" >&2
    exit 2
  fi
  sleep 0.1
done
if [ -z "$socket" ] || [ ! -S "$XDG_RUNTIME_DIR/$socket" ]; then
  echo "headless-sway: sway did not start" >&2
  tail -50 "$dir/sway.log" >&2
  exit 2
fi

# Its IPC socket is sway-ipc.<uid>.<pid>.sock in XDG_RUNTIME_DIR.
ipc="$XDG_RUNTIME_DIR/sway-ipc.$(id -u).$sway_pid.sock"
for _ in $(seq 1 50); do
  [ -S "$ipc" ] && break
  sleep 0.1
done

set +e
env -u DISPLAY WAYLAND_DISPLAY="$socket" SWAYSOCK="$ipc" "$@"
status=$?
set -e
exit "$status"
