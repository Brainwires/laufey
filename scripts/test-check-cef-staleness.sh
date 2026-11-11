#!/usr/bin/env bash
#
# Offline tests for scripts/check-cef-staleness.sh: each case feeds the gate
# a saved Chrome stable answer (CHROME_STABLE_JSON), a fixed pin, a fixed
# date and an optional waiver, and checks its exit status and annotation. No
# network.
#
#   scripts/test-check-cef-staleness.sh [path/to/check-cef-staleness.sh]
set -euo pipefail
cd "$(dirname "$0")/.."

CHECK=${1:-scripts/check-cef-staleness.sh}
PIN=156.0.15+gcf671c0+chromium-156.0.8078.80
TODAY=2026-11-10
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failures=0

# A versionhistory answer (the shape the gate fetches) listing the arguments.
answer() {
  local file=$tmp/answer.json sep="" v
  {
    printf '{\n  "versions": ['
    for v in "$@"; do
      printf '%s\n    {\n      "name": "chrome/platforms/win/channels/stable/versions/%s",\n      "version": "%s"\n    }' "$sep" "$v" "$v"
      sep=,
    done
    printf '\n  ],\n  "nextPageToken": "x"\n}\n'
  } >"$file"
  echo "$file"
}

# waiver <pin> <expires> [reason]: writes a waiver file and prints its path.
waiver() {
  local file=$tmp/waiver
  printf 'pin: %s\nexpires: %s\nreason: %s\n' "$1" "$2" "${3-blocked by a test}" >"$file"
  echo "$file"
}

# expect <name> <exit status> <output pattern> <answer> [waiver]
expect() {
  local name=$1 want=$2 pattern=$3 json=$4 waiver_file=${5:-$tmp/no-waiver}
  local out status=0
  out=$(CHROME_STABLE_JSON=$json CEF_PINNED_VERSION=$PIN \
    CEF_STALENESS_WAIVER=$waiver_file CEF_STALENESS_TODAY=$TODAY \
    bash "$CHECK" 2>&1) || status=$?
  if [ "$status" -ne "$want" ] || ! grep -Eq -- "$pattern" <<<"$out"; then
    echo "FAIL $name: exit $status (want $want), output:"
    printf '%s\n' "$out"
    failures=$((failures + 1))
  else
    echo "ok   $name"
  fi
}

expect "fresh: same milestone passes" 0 ': ok$' \
  "$(answer 156.0.8078.80 156.0.8078.73)"
expect "fresh: a newer build on the milestone is a notice" 0 '^::notice::.*156\.0\.8078\.120' \
  "$(answer 156.0.8078.120 156.0.8078.80)"
expect "fresh: one major behind passes" 0 ': ok$' \
  "$(answer 157.0.8100.20 156.0.8078.120)"
expect "warn window: two majors behind passes with a warning" 0 '^::warning::CEF is getting stale' \
  "$(answer 158.0.8200.5 157.0.8100.90)"
expect "stale: three majors behind fails" 1 '^::error::CEF is stale' \
  "$(answer 159.0.8300.1 158.0.8200.70)"
expect "stale: the newest version in the answer counts, not the first" 1 '^::error::CEF is stale.*160\.0\.8400\.2' \
  "$(answer 158.0.8200.70 160.0.8400.2 159.0.8300.1)"
expect "stale + valid waiver passes with a warning" 0 '^::warning::CEF is stale, waived until 2026-11-30' \
  "$(answer 159.0.8300.1)" "$(waiver "$PIN" 2026-11-30)"
expect "stale + waiver expiring today passes" 0 '^::warning::CEF is stale, waived' \
  "$(answer 159.0.8300.1)" "$(waiver "$PIN" "$TODAY")"
expect "stale + expired waiver fails" 1 '^::error::CEF staleness waiver expired on 2026-11-09' \
  "$(answer 159.0.8300.1)" "$(waiver "$PIN" 2026-11-09)"
expect "stale + waiver longer than 30 days fails" 1 '^::error::.*more than 30 days' \
  "$(answer 159.0.8300.1)" "$(waiver "$PIN" 2026-12-11)"
expect "stale + waiver for another pin fails" 1 '^::error::CEF is stale' \
  "$(answer 159.0.8300.1)" "$(waiver 150.0.21+g1+chromium-150.0.7871.255 2026-11-30)"
expect "stale + waiver without a reason fails" 1 "^::error::.*needs 'pin:'" \
  "$(answer 159.0.8300.1)" "$(waiver "$PIN" 2026-11-30 "")"
expect "fresh + a leftover waiver passes" 0 ': ok$' \
  "$(answer 156.0.8078.80)" "$(waiver "$PIN" 2026-11-01)"
printf '{"error": "unavailable"}\n' >"$tmp/broken.json"
expect "an answer without versions passes with a warning" 0 '^::warning::.*not checked' \
  "$tmp/broken.json"

# The repository's own pin: readable, and the same in cef/flake.nix.
repo_major=$(sed -nE 's/^CEF_FULL_VERSION := .*chromium-([0-9]+)\..*/\1/p' Makefile)
status=0
out=$(CHROME_STABLE_VERSION="$repo_major.0.0.1" bash "$CHECK" 2>&1) || status=$?
if [ "$status" -ne 0 ]; then
  echo "FAIL the repository's pin: exit $status, output:"
  printf '%s\n' "$out"
  failures=$((failures + 1))
else
  echo "ok   the repository's pin is readable and matches cef/flake.nix"
fi

if [ "$failures" -ne 0 ]; then
  echo "$failures case(s) failed"
  exit 1
fi
echo "all cases passed"
