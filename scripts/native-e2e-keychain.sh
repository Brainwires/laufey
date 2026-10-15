#!/usr/bin/env bash
# CEF on macOS: the cookie key's keychain item (laufey_cef_keychain.h), as
# launches of one profile through native-e2e-run.sh (keychain_checks.rs):
#
#   1. under the mock keychain (the default for this unsigned build), a page
#      sets a persistent cookie: cookieEncryption "basic";
#   2. with the app's own item (LAUFEY_TEST_KEYCHAIN_APP_SCOPED=1 stands in
#      for a team-signed build), the cookie is still there: the profile was
#      migrated and the CEF framework's SecItem* calls renamed to the
#      "<app id> Safe Storage" item: cookieEncryption "os";
#   3. the item exists under that service and account, and no
#      "Chromium Safe Storage" item appeared;
#   4. a second launch finds the item and still reads the cookie;
#   5. another program (laufey_secret_store_mac_reader, prompts off) is
#      refused the item, and so is another app id's launch, which gets an
#      item of its own.
#
# It uses the login keychain (CI's runner has an unlocked one) and deletes
# what it created. Never run it on a desktop where the login keychain matters
# to you: it adds and removes test items.
#
#   scripts/native-e2e-keychain.sh

set -euo pipefail
cd "$(dirname "$0")/.."

if [ "$(uname -s)" != Darwin ]; then
  echo "[e2e] N/A  keychain item (macOS only)"
  exit 0
fi

id="dev.laufey.e2e.keychain.$$"
id2="dev.laufey.e2e.keychain2.$$"
data="$(mktemp -d "${TMPDIR:-/tmp}/laufey-keychain-e2e.XXXXXX")"
data2="$(mktemp -d "${TMPDIR:-/tmp}/laufey-keychain-e2e2.XXXXXX")"
reader="$(ls cef/build/backend-common/laufey_secret_store_mac_reader \
  webview/build/backend-common/laufey_secret_store_mac_reader 2>/dev/null |
  head -1 || true)"
status=0

pass() { echo "[e2e] PASS $*"; }
fail() {
  echo "[e2e] FAIL $*"
  status=1
}

has_item() {
  /usr/bin/security find-generic-password -s "$1" -a "$2" >/dev/null 2>&1
}

cleanup() {
  # The host created the items; deleting a generic password the user owns
  # needs no prompt.
  /usr/bin/security delete-generic-password -s "$id Safe Storage" -a "$id" \
    >/dev/null 2>&1 || true
  /usr/bin/security delete-generic-password -s "$id2 Safe Storage" -a "$id2" \
    >/dev/null 2>&1 || true
  rm -rf "$data" "$data2"
}
trap cleanup EXIT

chromium_before=0
if has_item "Chromium Safe Storage" "Chromium"; then chromium_before=1; fi

launch() { # <phase> <expect> [extra env...]
  local phase=$1 expect=$2
  shift 2
  env LAUFEY_E2E_KEYCHAIN_PHASE="$phase" LAUFEY_E2E_KEYCHAIN_EXPECT="$expect" \
    "$@" scripts/native-e2e-run.sh cef --keychain
}

echo "== 1. mock keychain: set a cookie =="
launch set basic LAUFEY_APP_ID="$id" LAUFEY_DATA_DIR="$data" ||
  fail "launch 1 (mock keychain)"

echo "== 2. the app's own item: the profile is migrated =="
launch check os LAUFEY_APP_ID="$id" LAUFEY_DATA_DIR="$data" \
  LAUFEY_TEST_KEYCHAIN_APP_SCOPED=1 || fail "launch 2 (migration)"

if has_item "$id Safe Storage" "$id"; then
  pass "the item is \"$id Safe Storage\" (account \"$id\")"
else
  fail "no keychain item \"$id Safe Storage\" (account \"$id\")"
fi
if [ "$chromium_before" = 0 ] && has_item "Chromium Safe Storage" "Chromium"; then
  fail "a shared \"Chromium Safe Storage\" item appeared"
else
  pass "no shared \"Chromium Safe Storage\" item was created"
fi

echo "== 3. a second launch reads the cookie through the item =="
launch check os LAUFEY_APP_ID="$id" LAUFEY_DATA_DIR="$data" \
  LAUFEY_TEST_KEYCHAIN_APP_SCOPED=1 || fail "launch 3 (the item exists)"

echo "== 4. another app id gets an item of its own =="
launch set os LAUFEY_APP_ID="$id2" LAUFEY_DATA_DIR="$data2" \
  LAUFEY_TEST_KEYCHAIN_APP_SCOPED=1 || fail "launch 4 (another app id)"
if has_item "$id2 Safe Storage" "$id2"; then
  pass "another app id has its own item \"$id2 Safe Storage\""
else
  fail "no keychain item for the other app id"
fi

if [ -n "$reader" ]; then
  out="$("$reader" "$id Safe Storage" "$id" || true)"
  case "$out" in
    *"status 0"*) fail "another program read the app's item: $out" ;;
    *) pass "another program is refused the app's item ($out)" ;;
  esac
else
  fail "no laufey_secret_store_mac_reader (build the backend's tests)"
fi

exit "$status"
