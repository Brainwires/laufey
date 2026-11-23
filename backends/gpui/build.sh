#!/usr/bin/env bash
# Run a cargo command on the GPUI backend's workspace (the host `laufey_gpui`, the NTB1
# protocol crate and the self-test runtime).
#
#   backends/gpui/build.sh                                  # cargo build --release --workspace
#   backends/gpui/build.sh test -p laufey-ntb1 --locked     # the protocol's unit tests
#   backends/gpui/build.sh self-test                        # build, then laufey_gpui --self-test
#
# macOS: GPUI compiles its Metal shaders with Xcode's `metal` tool at build time. Where that
# tool is missing (Command Line Tools only, or Xcode 26 without the Metal toolchain), the
# `runtime-shaders` feature is added and the shaders are compiled at launch. The feature is
# the host package's, so it is only added to commands that select that package.
#
# The `laufey` crate (../../capi) binds laufey.h with bindgen, which needs libclang.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
cd "$here"

features=()
if [ "$(uname -s)" = Darwin ] && ! xcrun -f metal >/dev/null 2>&1; then
  features=(--features laufey-gpui/runtime-shaders)
fi

if [ "$#" -eq 0 ]; then set -- build --release --workspace; fi
command="$1"
shift

if [ "$command" = self-test ]; then
  cargo build --release --workspace ${features[@]+"${features[@]}"} "$@"
  bin=target/release/laufey_gpui
  [ -f "$bin.exe" ] && bin="$bin.exe"
  exec "$bin" --self-test
fi

# `--features laufey-gpui/...` needs the host package among the selected ones.
case " $* " in
  *" -p "* | *" --package "*)
    case " $* " in
      *" laufey-gpui "*) ;;
      *) features=() ;;
    esac
    ;;
esac
exec cargo "$command" ${features[@]+"${features[@]}"} "$@"
