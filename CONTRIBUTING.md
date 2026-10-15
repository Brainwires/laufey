# Contributing

Build and test instructions are in the [README](README.md) and
[docs/e2e-testing.md](docs/e2e-testing.md). This file lists the checks CI cannot
run.

## Manual checks

### CEF on macOS: a team-signed app's Safe Storage item

CI has no Apple Developer identity, so `scripts/native-e2e-keychain.sh` stands
in for a team-signed build with `LAUFEY_TEST_KEYCHAIN_APP_SCOPED=1`. Before a
release that changes `laufey_cef_keychain.h`, `cef_keychain_mac.mm`,
`cef_keychain_interpose_mac.mm` or the CEF version, check the real signing path
on a Mac with a Developer ID Application (or Apple Development) identity in its
login keychain:

1. Build the CEF backend and the native e2e runtime (`make cef`,
   `cargo build --release -p native_e2e_runtime`).
2. Sign the host bundle with the team identity, hardened runtime on:
   `codesign --force --deep --options runtime --sign "Developer ID Application: <Team>" cef/build/Release/laufey.app`.
   `codesign -dv cef/build/Release/laufey.app 2>&1 | grep TeamIdentifier` must
   print the team id (not `not set`).
3. Run one launch under the mock keychain first, without the test override, but
   with an unsigned copy of the build, so the profile holds mock-key cookies:
   `LAUFEY_E2E_KEYCHAIN_PHASE=set LAUFEY_E2E_KEYCHAIN_EXPECT=basic` with a fixed
   `LAUFEY_APP_ID=dev.laufey.manual.keychain` and
   `LAUFEY_DATA_DIR=<a new directory>`, through
   `scripts/native-e2e-run.sh cef --keychain`.
4. Run the signed build on the same profile with
   `LAUFEY_E2E_KEYCHAIN_PHASE=check LAUFEY_E2E_KEYCHAIN_EXPECT=os` and no
   `LAUFEY_TEST_KEYCHAIN_APP_SCOPED`: it must pass (the profile migrated, the
   cookie read through the app's item), stderr must say
   `cookies are now encrypted with the keychain item "dev.laufey.manual.keychain Safe Storage"`,
   and no Keychain prompt may appear.
5. `security find-generic-password -s "dev.laufey.manual.keychain Safe Storage" -a dev.laufey.manual.keychain`
   lists the item (without `-w`: no secret is read), and
   `security find-generic-password -s "Chromium Safe Storage"` finds no item
   this run created.
6. Rebuild, sign again with the same identity, and repeat step 4: no prompt, the
   cookie is still there (the item's access list names the designated
   requirement, which the team signature keeps).
7. Sign a copy ad-hoc (`codesign --force --deep --sign - …`) and run it on a new
   profile: `cookieEncryption` must be `"basic"` (the mock keychain), and no
   item named after the app id may be created.
8. Delete the test item:
   `security delete-generic-password -s "dev.laufey.manual.keychain Safe Storage" -a dev.laufey.manual.keychain`.
