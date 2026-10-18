#!/usr/bin/env bash
#
# CEF-update tripwire for Chromium's auto de-elevation (docs/backends.md, "The
# Chromium sandbox", the paragraph on apps started elevated).
#
# The Windows CEF host keeps an elevated app elevated by appending
# --do-not-de-elevate in its command line hook (CefWindowsBrowserSwitches in
# backend-common/include/laufey_launch_args.h). Chromium 149 reads the switch
# in ChromeBrowserMainPartsWin::PreEarlyInitialization, after that hook.
# Chromium main@{#1700144} (c4486a503a, "Move Windows auto-de-elevation check
# earlier in startup", first in Chromium 156) reads it in
# ChromeMainDelegate::BasicStartupComplete, which CEF runs BEFORE the hook, so
# with that Chromium the switch comes too late: an app started elevated exits
# 1 at once, its relaunch loses the launcher's environment, and a cold-start
# deep link is lost. This check fails once the pinned CEF is on Chromium 156
# or later, until the switch reaches the command line before CEF parses it
# (a CEF change or patch; see the doc paragraph) and SWITCH_BEFORE_PARSE below
# is set to 1.
#
#   scripts/check-cef-de-elevation.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# The first Chromium major that reads the switch before the hook.
FIRST_EARLY_CHECK=156
# 1 once the host puts --do-not-de-elevate on the command line before CEF
# parses it.
SWITCH_BEFORE_PARSE=0

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
  line hook, so --do-not-de-elevate (CefWindowsBrowserSwitches) no longer
  keeps an elevated Windows app elevated. Move the switch ahead of CEF's
  command line parsing first; see docs/backends.md ("The Chromium sandbox",
  apps started elevated).
MSG
    status=1
  else
    echo "check-cef-de-elevation: $file pins $version: ok"
  fi
done
exit "$status"
