#!/usr/bin/env bash
#
# CEF-update tripwire for Chromium's auto de-elevation (docs/backends.md, "The
# Chromium sandbox", the paragraph on apps started elevated).
#
# The Windows CEF host keeps an elevated app elevated with
# --do-not-de-elevate. Chromium 149 and 150 read the switch in
# ChromeBrowserMainPartsWin::PreEarlyInitialization, after the host's command
# line hook (CefWindowsBrowserSwitches in
# backend-common/include/laufey_launch_args.h), which appends it.
# Chromium main@{#1700144} (c4486a503a, "Move Windows auto-de-elevation check
# earlier in startup", first in Chromium 156) reads it in
# ChromeMainDelegate::BasicStartupComplete, which CEF runs BEFORE the hook, so
# there the hook's switch comes too late: an app started elevated would exit
# 1 at once, its relaunch would lose the launcher's environment, and a
# cold-start deep link would be lost. For Chromium 156+ the host puts the
# switch on the command line before CEF parses it: an elevated browser
# process without it starts itself again with it (NeedsDeElevationRelaunch,
# LaufeyRelaunchKeepingElevation in cef/src/main_windows.cc; checked at
# 156.0.8078.48), so SWITCH_BEFORE_PARSE below is 1. This check fails on a
# Chromium 156+ pin when SWITCH_BEFORE_PARSE is 0 or the relaunch is gone from
# cef/src/main_windows.cc.
#
#   scripts/check-cef-de-elevation.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# The first Chromium major that reads the switch before the hook.
FIRST_EARLY_CHECK=156
# 1 once the host puts --do-not-de-elevate on the command line before CEF
# parses it.
SWITCH_BEFORE_PARSE=1

# The relaunch that puts the switch on the command line first.
if [ "$SWITCH_BEFORE_PARSE" = 1 ] &&
  ! grep -q 'NeedsDeElevationRelaunch' cef/src/main_windows.cc; then
  echo "check-cef-de-elevation: SWITCH_BEFORE_PARSE=1, but cef/src/main_windows.cc no longer calls NeedsDeElevationRelaunch" >&2
  SWITCH_BEFORE_PARSE=0
fi

status=0
for file in Makefile cef/flake.nix; do
  version=$(grep -Eo 'chromium-[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' "$file" | head -1)
  if [ -z "$version" ]; then
    echo "check-cef-de-elevation: no pinned chromium-<version> in $file" >&2
    status=1
    continue
  fi
  major=${version#chromium-}
  major=${major%%.*}
  if [ "$major" -ge "$FIRST_EARLY_CHECK" ] && [ "$SWITCH_BEFORE_PARSE" != 1 ]; then
    cat >&2 <<MSG
check-cef-de-elevation: $file pins $version (Chromium $major).
  Chromium $FIRST_EARLY_CHECK+ checks auto de-elevation before CEF's command
  line hook, so --do-not-de-elevate (CefWindowsBrowserSwitches) alone no
  longer keeps an elevated Windows app elevated. The switch must reach the
  command line before CEF parses it (NeedsDeElevationRelaunch); see
  docs/backends.md ("The Chromium sandbox", apps started elevated).
MSG
    status=1
  else
    echo "check-cef-de-elevation: $file pins $version: ok"
  fi
done
exit "$status"
