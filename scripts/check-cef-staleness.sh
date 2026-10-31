#!/usr/bin/env bash
#
# CEF staleness warning (docs/backends.md, "Updating CEF").
#
# Chromium fixes security bugs on the current stable milestone (and Chrome's
# Extended Stable one) only, so a pinned CEF that trails Chrome stable stops
# getting fixes for bugs any page can reach. This prints a GitHub Actions
# `::warning::` when the Chromium major pinned in the Makefile
# (CEF_FULL_VERSION's `chromium-<major>.`) is more than ALLOWED_LAG majors
# behind Chrome stable. It never fails the run: an old pin is a reason to
# update CEF, not to stop CI. When Chrome stable can't be learned (offline,
# an HTTP error, an answer it can't read) it says so and passes; a pin it
# can't read fails, since that is a bug in this repository.
#
#   scripts/check-cef-staleness.sh
#   CHROME_STABLE_MAJOR=160 scripts/check-cef-staleness.sh   # no network
set -euo pipefail
cd "$(dirname "$0")/.."

# How many Chrome stable majors the pin may trail before it warns.
ALLOWED_LAG=2
URL=${CHROME_STABLE_URL:-https://versionhistory.googleapis.com/v1/chrome/platforms/win/channels/stable/versions?pageSize=10}

version=$(grep -Eo 'chromium-[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' Makefile | head -1 || true)
if [ -z "$version" ]; then
  echo "check-cef-staleness: no pinned chromium-<version> in Makefile" >&2
  exit 1
fi
pinned=${version#chromium-}
pinned=${pinned%%.*}

stable=${CHROME_STABLE_MAJOR:-}
if [ -z "$stable" ]; then
  if ! body=$(curl -fsS --max-time 15 "$URL" 2>&1); then
    echo "::warning::check-cef-staleness: Chrome stable unknown ($body); CEF staleness not checked"
    exit 0
  fi
  stable=$(printf '%s' "$body" | grep -Eo '"version": *"[0-9]+\.' |
    grep -Eo '[0-9]+' | sort -n | tail -1 || true)
  if [ -z "$stable" ]; then
    echo "::warning::check-cef-staleness: no Chrome stable version in the answer from $URL; CEF staleness not checked"
    exit 0
  fi
fi

behind=$((stable - pinned))
if [ "$behind" -gt "$ALLOWED_LAG" ]; then
  echo "::warning::CEF is $behind Chrome majors behind stable (pinned $version, Chrome stable $stable; at most $ALLOWED_LAG allowed). Update CEF: docs/backends.md, \"Updating CEF\"."
else
  echo "check-cef-staleness: Makefile pins $version, Chrome stable $stable: ok"
fi
