#!/usr/bin/env bash
#
# CEF staleness gate (docs/backends.md, "CEF bump cadence").
#
# Chromium fixes security bugs on the current stable milestone (and Chrome's
# Extended Stable one) only, so a pinned CEF that trails Chrome stable stops
# getting fixes for bugs any page can reach. This compares the Chromium pinned
# in the Makefile (CEF_FULL_VERSION's `chromium-<version>`) with the newest
# Chrome stable release and decides by how many majors the pin trails it:
#
#   behind < WARN_LAG     ok (CEF ships a milestone a little after Chrome)
#   behind == WARN_LAG    a GitHub Actions `::warning::`; the run passes
#   behind >= ERROR_LAG   an `::error::` and exit 1: the run fails
#
# On the same major, a newer Chrome stable build than the pin's prints a
# `::notice::` (Chrome shipped fixes the pin lacks; the next CEF build on the
# branch carries them).
#
# A known-blocked bump can be waived for a short while with a waiver file
# (cef/staleness-waiver): it turns the error into a warning only while it
# names the pinned CEF_FULL_VERSION exactly and its `expires:` date has not
# passed. An expired or malformed waiver, or one that expires more than
# MAX_WAIVER_DAYS from today, fails the run; a waiver for another pin is
# ignored with a notice (remove it). The file holds three lines:
#
#   pin: 156.0.28+g57389f0+chromium-156.0.8078.88
#   expires: 2026-12-01
#   reason: <why the bump is blocked, with a link to the tracking issue>
#
# When Chrome stable can't be learned (offline, an HTTP error, an answer it
# can't read) it says so and passes; a pin it can't read, or a cef/flake.nix
# pin that differs from the Makefile's, fails, since that is a bug in this
# repository.
#
#   scripts/check-cef-staleness.sh
#
# Offline inputs (scripts/test-check-cef-staleness.sh uses them):
#   CHROME_STABLE_JSON=<file>          a saved versionhistory answer
#   CHROME_STABLE_VERSION=160.0.1.2    Chrome stable itself
#   CEF_PINNED_VERSION=<CEF_FULL_VERSION>  instead of the Makefile's pin
#   CEF_STALENESS_WAIVER=<file>        instead of cef/staleness-waiver
#   CEF_STALENESS_TODAY=YYYY-MM-DD     instead of today's UTC date
set -euo pipefail
cd "$(dirname "$0")/.."

# How many Chrome stable majors behind warns, and how many fail the run.
WARN_LAG=2
ERROR_LAG=3
# The longest a waiver may run, counted from today.
MAX_WAIVER_DAYS=30
WAIVER=${CEF_STALENESS_WAIVER:-cef/staleness-waiver}
URL=${CHROME_STABLE_URL:-https://versionhistory.googleapis.com/v1/chrome/platforms/win/channels/stable/versions?pageSize=10}
DOC='docs/backends.md, "CEF bump cadence"'

# Days since the epoch for a YYYY-MM-DD date (GNU date, then BSD date).
day_number() {
  local s
  s=$(date -u -d "$1 00:00:00" +%s 2>/dev/null ||
    date -u -j -f '%Y-%m-%d %H:%M:%S' "$1 00:00:00" +%s 2>/dev/null) || return 1
  echo $((s / 86400))
}

# The newest a.b.c.d version among stdin's lines.
newest_version() {
  sort -t. -k1,1n -k2,2n -k3,3n -k4,4n | tail -1
}

full=${CEF_PINNED_VERSION:-}
if [ -z "$full" ]; then
  full=$(sed -nE 's/^CEF_FULL_VERSION := *([^ ]+) *$/\1/p' Makefile | head -1)
  flake=$(sed -nE 's/^ *cefFullVersion = "([^"]+)";.*/\1/p' cef/flake.nix | head -1)
  if [ -n "$full" ] && [ "$flake" != "$full" ]; then
    echo "::error::check-cef-staleness: cef/flake.nix pins CEF '$flake', the Makefile '$full'; update both ($DOC)"
    exit 1
  fi
fi
pinned_version=$(printf '%s' "$full" | grep -Eo 'chromium-[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' || true)
if [ -z "$pinned_version" ]; then
  echo "check-cef-staleness: no pinned chromium-<version> in CEF_FULL_VERSION '$full'" >&2
  exit 1
fi
pinned_version=${pinned_version#chromium-}
pinned=${pinned_version%%.*}

stable_version=${CHROME_STABLE_VERSION:-}
if [ -z "$stable_version" ]; then
  if [ -n "${CHROME_STABLE_JSON:-}" ]; then
    body=$(cat "$CHROME_STABLE_JSON")
  elif ! body=$(curl -fsS --max-time 15 "$URL" 2>&1); then
    echo "::warning::check-cef-staleness: Chrome stable unknown ($body); CEF staleness not checked"
    exit 0
  fi
  stable_version=$(printf '%s' "$body" |
    grep -Eo '"version": *"[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+"' |
    grep -Eo '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' | newest_version || true)
  if [ -z "$stable_version" ]; then
    echo "::warning::check-cef-staleness: no Chrome stable version in the answer from $URL; CEF staleness not checked"
    exit 0
  fi
fi
stable=${stable_version%%.*}

behind=$((stable - pinned))
summary="pinned CEF $full, Chrome stable $stable_version: $behind majors behind"

# Exits 0 (with a warning) for a waiver that covers this pin, exits 1 for an
# expired, too long or malformed one, and returns for none.
check_waiver() {
  [ -f "$WAIVER" ] || return 0
  local pin expires reason today today_day expires_day
  pin=$(sed -nE 's/^pin: *([^ ]+) *$/\1/p' "$WAIVER" | head -1)
  expires=$(sed -nE 's/^expires: *([0-9]{4}-[0-9]{2}-[0-9]{2}) *$/\1/p' "$WAIVER" | head -1)
  reason=$(sed -nE 's/^reason: *(.*[^ ]) *$/\1/p' "$WAIVER" | head -1)
  if [ -z "$pin" ] || [ -z "$expires" ] || [ -z "$reason" ]; then
    echo "::error::check-cef-staleness: $WAIVER needs 'pin:', 'expires: YYYY-MM-DD' and 'reason:' lines ($DOC)"
    exit 1
  fi
  if [ "$pin" != "$full" ]; then
    echo "::notice::check-cef-staleness: $WAIVER is for CEF $pin, not the pinned $full; it is ignored (remove it)"
    return 0
  fi
  today=${CEF_STALENESS_TODAY:-$(date -u +%Y-%m-%d)}
  if ! today_day=$(day_number "$today") || ! expires_day=$(day_number "$expires"); then
    echo "::error::check-cef-staleness: can't read the dates '$today' / '$expires' ($WAIVER)"
    exit 1
  fi
  if [ "$today_day" -gt "$expires_day" ]; then
    echo "::error::CEF staleness waiver expired on $expires; $summary (fails at $ERROR_LAG). Update CEF, or renew $WAIVER for at most $MAX_WAIVER_DAYS days ($DOC)."
    exit 1
  fi
  if [ $((expires_day - today_day)) -gt "$MAX_WAIVER_DAYS" ]; then
    echo "::error::check-cef-staleness: $WAIVER expires $expires, more than $MAX_WAIVER_DAYS days after $today ($DOC)"
    exit 1
  fi
  echo "::warning::CEF is stale, waived until $expires by $WAIVER ($reason); $summary (fails at $ERROR_LAG)."
  exit 0
}

if [ "$behind" -ge "$ERROR_LAG" ]; then
  check_waiver
  echo "::error::CEF is stale: $summary (fails at $ERROR_LAG). Update CEF ($DOC), or add a dated $WAIVER if the bump is blocked."
  exit 1
elif [ "$behind" -ge "$WARN_LAG" ]; then
  echo "::warning::CEF is getting stale: $summary (warns at $WARN_LAG, fails at $ERROR_LAG). Update CEF ($DOC)."
else
  if [ "$behind" -eq 0 ] &&
    [ "$(printf '%s\n%s\n' "$pinned_version" "$stable_version" | newest_version)" != "$pinned_version" ]; then
    echo "::notice::check-cef-staleness: Chrome stable $stable_version is newer than the pinned Chromium $pinned_version on the same milestone; the next CEF build on the branch carries its fixes ($DOC)."
  fi
  echo "check-cef-staleness: $summary: ok"
fi
