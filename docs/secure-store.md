# Secure store

A small secret per (service, account) in the OS's secret store (API 47):

```rust
use std::time::Duration;
let t = Duration::from_secs(20);
laufey::secret_store("com.example.app", "refresh-token", "Example", "s3cret", t)?;
let value = laufey::secret_lookup("com.example.app", "refresh-token", t)?; // Some("s3cret")
laufey::secret_delete("com.example.app", "refresh-token", t)?;
```

The C ABI entry points are `secret_lookup`, `secret_store` and `secret_delete`.
Each returns a `LAUFEY_SECRET_*` status (`OK`, `NOT_FOUND`, `UNAVAILABLE`,
`FAILED`) with the value and a reason as strings freed with `string_free`. Each
**blocks the calling thread** for at most about its timeout (0: 20 s): call it
off the UI thread.

They exist on Linux, in the CEF and WebView backends. On macOS and Windows (and
on Winit) they are `NULL`: the embedder uses the Keychain / Credential Locker
itself.

## Linux

The secret lives in the Secret Service (`org.freedesktop.secrets`:
gnome-keyring, KWallet's Secret Service, KeePassXC), read and written through
libsecret (`libsecret-1.so.0`, loaded at run time: no build dependency, and
every desktop ships it). Items carry the attributes `service` and `account`, the
shape `secret-tool store --label=… service S account A` makes, so either reads
the other's items; a store replaces what is there (and clears duplicates written
before).

Around the secret, plain D-Bus reads that never prompt decide what can happen:

| Situation                                                                                                                                                                      | Answer                                                                                                                                                |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| No session bus                                                                                                                                                                 | `UNAVAILABLE`, at once.                                                                                                                               |
| libsecret isn't installed                                                                                                                                                      | `UNAVAILABLE`, naming the package (`libsecret-1-0` on Debian / Ubuntu, `libsecret` on Fedora).                                                        |
| No provider, and KWallet runs without serving the Secret Service                                                                                                               | `UNAVAILABLE`: "enable KWallet's Secret Service (System Settings > KWallet: Use KWallet for the Secret Service interface), or install gnome-keyring". |
| No provider at all                                                                                                                                                             | `UNAVAILABLE`: "install gnome-keyring, or enable KWallet's Secret Service".                                                                           |
| A matching item (or, for a write, the default collection) is locked, and no one here can answer an unlock prompt (no graphical session, or gnome-keyring without its prompter) | `UNAVAILABLE` at once, naming the keyring ("the gnome-keyring keyring is locked …", "the KWallet wallet is closed …"). Never `NOT_FOUND`.             |
| Locked, and someone could answer                                                                                                                                               | libsecret asks to unlock. An answer in time goes on; a dismissed prompt or none within the timeout is `UNAVAILABLE`. Never a hang.                    |
| The provider doesn't answer                                                                                                                                                    | `UNAVAILABLE` after the timeout.                                                                                                                      |
| Nothing matches (and nothing locked does)                                                                                                                                      | `NOT_FOUND` (a lookup); `OK` (a delete).                                                                                                              |

There is never a plaintext fallback. "Someone could answer an unlock prompt" is
`platform_features`' `secretServicePrompt` (see
[platform-features.md](platform-features.md)).

## Testing

- `laufey_secret_store_dbus_test` (ctest, Linux): on a private bus, libsecret
  missing, no session bus, no provider, KWallet without the Secret Service, a
  provider that never answers, a locked item where no one can answer (refused at
  once for lookup, store and delete), and a real `gnome-keyring-daemon`: the
  round trips, then locked without a prompter (at once) and with a prompter that
  never answers (after the timeout).
- `scripts/native-e2e-run.sh <backend> --secret-store`: every call answers
  within its timeout through the backend;
  `LAUFEY_E2E_EXPECT_SECRET=ok|unavailable` for a real session.
