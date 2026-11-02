#!/usr/bin/env bash
# Installs the build tools a Windows CI job needs: GNU make and ninja, at pinned
# versions, each checked against its SHA-256 before use, on the job's PATH for
# the steps after this one.
#
# Not `choco install`: that asks Chocolatey's package feed for the version, and
# the feed's 504s failed setup on healthy runners. The same make package comes
# straight from Chocolatey's CDN, with its upstream archive as the second
# source, and ninja from its own GitHub release.
set -euo pipefail

MAKE_VERSION=4.4.1
NINJA_VERSION=1.13.2

tools="${LAUFEY_TOOLS_DIR:-${RUNNER_TEMP:?}/laufey-tools}"
mkdir -p "$tools"

# fetch <dest> <url> <sha256>: true once <dest> holds exactly those bytes.
fetch() {
  rm -f "$1"
  if ! curl -fsSL --retry 4 --retry-all-errors --retry-delay 5 \
    --connect-timeout 20 --max-time 120 -o "$1" "$2"; then
    echo "download failed: $2" >&2
    return 1
  fi
  local got
  got="$(sha256sum "$1" | cut -d' ' -f1)"
  if [ "$got" != "$3" ]; then
    echo "SHA-256 mismatch for $2: got $got, expected $3" >&2
    rm -f "$1"
    return 1
  fi
}

# extract <zip> <dir>
extract() {
  if command -v unzip >/dev/null; then
    unzip -q -o "$1" -d "$2"
  else
    powershell -NoProfile -Command \
      "Expand-Archive -Force -Path '$(cygpath -w "$1")' -DestinationPath '$(cygpath -w "$2")'"
  fi
}

# install <exe name> <dir name>, then <url> <sha256> pairs tried in order.
install_tool() {
  local exe="$1" dir="$tools/$2"
  shift 2
  local zip="$tools/$exe.zip"
  while [ "$#" -ge 2 ]; do
    if fetch "$zip" "$1" "$2"; then
      rm -rf "$dir"
      extract "$zip" "$dir"
      rm -f "$zip"
      local found
      found="$(find "$dir" -type f -name "$exe" | head -1)"
      if [ -z "$found" ]; then
        echo "$exe is not in the archive from $1" >&2
        return 1
      fi
      path_add "$(dirname "$found")"
      echo "$exe from $1"
      return 0
    fi
    shift 2
  done
  echo "$exe: every source failed" >&2
  return 1
}

path_add() {
  local p="$1"
  if command -v cygpath >/dev/null; then
    p="$(cygpath -w "$p")"
  fi
  echo "$p" >>"${GITHUB_PATH:-/dev/null}"
}

install_tool make.exe "make-$MAKE_VERSION" \
  "https://packages.chocolatey.org/make.$MAKE_VERSION.nupkg" \
  8b2dec73ed4c84163c4a4f9bfe658553b1e3d1fdfa8930d2aa438e030ff21f9d \
  "https://downloads.sourceforge.net/project/ezwinports/make-$MAKE_VERSION-without-guile-w32-bin.zip" \
  fb66a02b530f7466f6222ce53c0b602c5288e601547a034e4156a512dd895ee7

install_tool ninja.exe "ninja-$NINJA_VERSION" \
  "https://github.com/ninja-build/ninja/releases/download/v$NINJA_VERSION/ninja-win.zip" \
  07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65
