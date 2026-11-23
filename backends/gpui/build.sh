#!/usr/bin/env bash
# Build (or run any cargo command on) the GPUI spike with the fork's prebuilt V8.
#
#   backends/gpui/build.sh                      # cargo build --release
#   backends/gpui/build.sh run --release -- --self-test
#
# What a build needs beyond `cargo`:
#
#   RUSTY_V8_ARCHIVE           librusty_v8_release_<target>.a.gz (rusty_v8.lib.gz on Windows)
#   RUSTY_V8_ARCHIVE_SHA256    its SHA-256; the v8 crate's build script checks it again
#   RUSTY_V8_SRC_BINDING_PATH  src_binding_release_<target>.rs
#
# Both files come from the Brainwires/rusty_v8 release named in rusty_v8.sha256 (a copy of the
# deno fork's manifest) and must match the hashes there. They are looked for in
# $RUSTY_V8_DIR (default ~/.cache/denext-rusty-v8, where the fork's fetch_rusty_v8.py puts
# them) and downloaded there when absent. V8 is never built from source here, and
# .cargo/config.toml refuses the build script's own download.
#
# macOS: GPUI compiles its Metal shaders with Xcode's `metal` tool at build time. Where that
# tool is missing (Command Line Tools only, or Xcode 26 without the Metal toolchain), the
# `runtime-shaders` feature is added and the shaders are compiled at launch.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
manifest="$here/rusty_v8.sha256"
target="${CARGO_BUILD_TARGET:-$(rustc -vV | sed -n 's/^host: //p')}"
dir="${RUSTY_V8_DIR:-$HOME/.cache/denext-rusty-v8}"

repo="$(sed -n 's/^# repo: //p' "$manifest")"
tag="$(sed -n 's/^# tag: //p' "$manifest")"
case "$target" in
  *windows*) lib="rusty_v8_release_$target.lib.gz" ;;
  *) lib="librusty_v8_release_$target.a.gz" ;;
esac
binding="src_binding_release_$target.rs"

sha256_of() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

pinned() {
  local want
  want="$(awk -v f="$1" '$2 == f { print $1 }' "$manifest")"
  [ -n "$want" ] || { echo "build.sh: rusty_v8.sha256 lists no $1" >&2; exit 1; }
  echo "$want"
}

# Leaves a verified $dir/$1 behind or exits.
verified() {
  local name="$1" want got
  want="$(pinned "$name")"
  if [ -f "$dir/$name" ] && [ "$(sha256_of "$dir/$name")" = "$want" ]; then return; fi
  mkdir -p "$dir"
  echo "build.sh: fetching $name from $repo $tag" >&2
  curl --fail --location --silent --show-error --retry 5 \
    --output "$dir/$name.part" "https://github.com/$repo/releases/download/$tag/$name"
  got="$(sha256_of "$dir/$name.part")"
  if [ "$got" != "$want" ]; then
    rm -f "$dir/$name.part"
    echo "build.sh: $name: SHA-256 $got, rusty_v8.sha256 pins $want" >&2
    exit 1
  fi
  mv "$dir/$name.part" "$dir/$name"
}

verified "$lib"
verified "$binding"
export RUSTY_V8_ARCHIVE="$dir/$lib"
export RUSTY_V8_ARCHIVE_SHA256="$(pinned "$lib")"
export RUSTY_V8_SRC_BINDING_PATH="$dir/$binding"

features=()
if [ "$(uname -s)" = Darwin ] && ! xcrun -f metal >/dev/null 2>&1; then
  features=(--features runtime-shaders)
fi

cd "$here"
if [ "$#" -eq 0 ]; then set -- build --release; fi
command="$1"
shift
exec cargo "$command" ${features[@]+"${features[@]}"} "$@"
