#!/usr/bin/env bash
# Build (or run any cargo command on) the GPUI spike with the fork's prebuilt V8.
#
#   backends/gpui/build.sh                      # cargo build --release
#   backends/gpui/build.sh run --release -- --self-test
#   backends/gpui/build.sh fetch [--seed <dir>] # fetch and verify V8 only, run no cargo
#
# What a build needs beyond `cargo`:
#
#   RUSTY_V8_ARCHIVE           librusty_v8_release_<target>.a.gz (rusty_v8_release_<target>.lib.gz
#                              on Windows)
#   RUSTY_V8_ARCHIVE_SHA256    its SHA-256; the v8 crate's build script checks it again
#   RUSTY_V8_SRC_BINDING_PATH  src_binding_release_<target>.rs
#
# Both files come from the Brainwires/rusty_v8 release named in rusty_v8.sha256 (a copy of the
# deno fork's manifest) and must match the hashes there. They are looked for in
# $RUSTY_V8_DIR (default ~/.cache/denext-rusty-v8, where the fork's fetch_rusty_v8.py puts
# them) and downloaded there when absent. V8 is never built from source here, and
# .cargo/config.toml refuses the build script's own download. The release tag must match
# Cargo.lock's `v8` version (156.0.0+denext.6 <-> v156.0.0-denext.6), as in the fork's script.
#
# The target is the host's, or $CARGO_BUILD_TARGET. Any target rusty_v8.sha256 lists works:
# aarch64/x86_64 apple-darwin, aarch64/x86_64 unknown-linux-gnu, aarch64/x86_64 pc-windows-msvc.
# On Windows this runs under Git Bash; the paths handed to cargo are converted with `cygpath`.
#
# `fetch --seed <dir>` also unpacks the verified library to <dir>/gn_out/obj/librusty_v8.a
# (rusty_v8.lib on Windows), where the v8 crate's build script leaves it; <dir> is the cargo
# profile directory (target/release). A restored build cache can keep the build script's
# fingerprint but not gn_out, and then the link finds no library (the fork's --seed-dir).
#
# macOS: GPUI compiles its Metal shaders with Xcode's `metal` tool at build time. Where that
# tool is missing (Command Line Tools only, or Xcode 26 without the Metal toolchain), the
# `runtime-shaders` feature is added and the shaders are compiled at launch.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
manifest="$here/rusty_v8.sha256"
target="${CARGO_BUILD_TARGET:-$(rustc -vV | sed -n 's/^host: //p')}"
dir="${RUSTY_V8_DIR:-$HOME/.cache/denext-rusty-v8}"

die() {
  echo "build.sh: $*" >&2
  exit 1
}

repo="$(sed -n 's/^# repo: //p' "$manifest")"
tag="$(sed -n 's/^# tag: //p' "$manifest")"
[ -n "$repo" ] && [ -n "$tag" ] || die "rusty_v8.sha256 names no repo or tag"
case "$target" in
  *windows*)
    lib="rusty_v8_release_$target.lib.gz"
    unpacked="rusty_v8.lib"
    ;;
  *)
    lib="librusty_v8_release_$target.a.gz"
    unpacked="librusty_v8.a"
    ;;
esac
binding="src_binding_release_$target.rs"

# The release tag must be v<version>-<metadata> for Cargo.lock's v8 <version>+<metadata>.
v8_version="$(awk '/^\[\[package\]\]/ { p = 0 } $0 == "name = \"v8\"" { p = 1 }
  p && /^version = / { gsub(/"/, "", $3); print $3; exit }' "$here/Cargo.lock")"
[ -n "$v8_version" ] || die "Cargo.lock has no v8 package"
case "$v8_version" in
  *+*) want_tag="v${v8_version%%+*}-${v8_version#*+}" ;;
  *) die "Cargo.lock's v8 is $v8_version, not a <version>+denext.<n> fork release" ;;
esac
[ "$tag" = "$want_tag" ] ||
  die "rusty_v8.sha256 names release $tag, but Cargo.lock's v8 is $v8_version (expected $want_tag)"

sha256_of() {
  if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1
  else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

pinned() {
  local want
  want="$(awk -v f="$1" '$2 == f { print $1 }' "$manifest")"
  [ -n "$want" ] || die "rusty_v8.sha256 lists no $1 (target $target)"
  echo "$want"
}

# Leaves a verified $dir/$1 behind or exits.
verified() {
  local name="$1" want got
  want="$(pinned "$name")"
  if [ -f "$dir/$name" ] && [ "$(sha256_of "$dir/$name")" = "$want" ]; then return; fi
  mkdir -p "$dir"
  echo "build.sh: fetching $name from $repo $tag" >&2
  curl --fail --location --silent --show-error --retry 5 --retry-all-errors \
    --output "$dir/$name.part" "https://github.com/$repo/releases/download/$tag/$name"
  got="$(sha256_of "$dir/$name.part")"
  if [ "$got" != "$want" ]; then
    rm -f "$dir/$name.part"
    die "$name: SHA-256 $got, rusty_v8.sha256 pins $want"
  fi
  mv "$dir/$name.part" "$dir/$name"
  echo "build.sh: $name: SHA-256 OK ($got)" >&2
}

# A path cargo and the build script (native Windows programs) can read.
native_path() {
  if command -v cygpath >/dev/null; then cygpath -m "$1"; else echo "$1"; fi
}

verified "$lib"
verified "$binding"

if [ "${1:-}" = fetch ]; then
  shift
  seed=""
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --seed)
        [ "$#" -ge 2 ] || die "--seed needs a directory"
        seed="$2"
        shift 2
        ;;
      *) die "fetch: unknown argument $1" ;;
    esac
  done
  if [ -n "$seed" ]; then
    mkdir -p "$seed/gn_out/obj"
    gzip -dc "$dir/$lib" >"$seed/gn_out/obj/$unpacked.part"
    mv "$seed/gn_out/obj/$unpacked.part" "$seed/gn_out/obj/$unpacked"
    echo "build.sh: seeded $seed/gn_out/obj/$unpacked" >&2
  fi
  echo "build.sh: $target: $dir/$lib and $dir/$binding verified against $tag" >&2
  exit 0
fi

RUSTY_V8_ARCHIVE="$(native_path "$dir/$lib")"
RUSTY_V8_ARCHIVE_SHA256="$(pinned "$lib")"
RUSTY_V8_SRC_BINDING_PATH="$(native_path "$dir/$binding")"
export RUSTY_V8_ARCHIVE RUSTY_V8_ARCHIVE_SHA256 RUSTY_V8_SRC_BINDING_PATH

features=()
if [ "$(uname -s)" = Darwin ] && ! xcrun -f metal >/dev/null 2>&1; then
  features=(--features runtime-shaders)
fi

cd "$here"
if [ "$#" -eq 0 ]; then set -- build --release; fi
command="$1"
shift
exec cargo "$command" ${features[@]+"${features[@]}"} "$@"
