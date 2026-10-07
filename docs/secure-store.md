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

| Situation                                                                                                                                                                      | Answer                                                                                                                                                                                                                                                                                                                                                                                                       |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| No session bus                                                                                                                                                                 | `UNAVAILABLE`, at once.                                                                                                                                                                                                                                                                                                                                                                                      |
| libsecret isn't installed                                                                                                                                                      | `UNAVAILABLE`, naming the package (`libsecret-1-0` on Debian / Ubuntu, `libsecret` on Fedora).                                                                                                                                                                                                                                                                                                               |
| No provider, and KWallet runs without serving the Secret Service                                                                                                               | `UNAVAILABLE`: "enable KWallet's Secret Service (System Settings > KWallet: Use KWallet for the Secret Service interface), or install gnome-keyring".                                                                                                                                                                                                                                                        |
| No provider at all                                                                                                                                                             | `UNAVAILABLE`: "install gnome-keyring, or enable KWallet's Secret Service".                                                                                                                                                                                                                                                                                                                                  |
| A matching item (or, for a write, the default collection) is locked, and no one here can answer an unlock prompt (no graphical session, or gnome-keyring without its prompter) | `UNAVAILABLE` at once, naming the keyring ("the gnome-keyring keyring is locked …", "the KWallet wallet is closed …"). Never `NOT_FOUND`.                                                                                                                                                                                                                                                                    |
| Locked, and someone could answer                                                                                                                                               | The Secret Service is asked to unlock (its own prompt, on a thread of its own) and the call waits up to its timeout: an answer in time goes on, a prompt closed or unanswered is `UNAVAILABLE`. Never a hang. The prompt is never dismissed from here: it stays up for the person to answer, and a later answer only unlocks (nothing is written after the call gave up). One unlock is in flight at a time. |
| No default keyring (a write)                                                                                                                                                   | `UNAVAILABLE`: creating one is the desktop's keyring manager's job.                                                                                                                                                                                                                                                                                                                                          |
| The provider doesn't answer                                                                                                                                                    | `UNAVAILABLE` after the timeout.                                                                                                                                                                                                                                                                                                                                                                             |
| Nothing matches (and nothing locked does)                                                                                                                                      | `NOT_FOUND` (a lookup); `OK` (a delete).                                                                                                                                                                                                                                                                                                                                                                     |

There is never a plaintext fallback.

Why the prompt is never dismissed: gnome-keyring (48) aborts when a client
dismisses its unlock prompt while it is up (`gkd-secret-unlock.c`
`perform_next_unlock`: assertion `!self->current`), which is what libsecret does
when a call it prompted for is cancelled. So the unlock is asked for directly
(`Service.Unlock`, then the prompt's `Prompt`), and libsecret runs only once
nothing is locked any more. "Someone could answer an unlock prompt" is
`platform_features`' `secretServicePrompt` (see
[platform-features.md](platform-features.md)).

When another daemon takes `org.freedesktop.secrets` over (a session running two
gnome-keyring daemons: PAM's `--login` one and a D-Bus-activated
`--components=secrets` one), the next call opens a new transfer session with it.
libsecret keeps one service, and the session it opened with the first owner, for
the process: it watches the name from the private main context of the `*_sync`
call that made the service, which is never iterated again, so it never notices
the change, and the new owner refused every later write ("The session wrapping
the secret does not exist", or "The secret was transferred or encrypted in an
invalid way"). Each call now notes the owner it is made with and drops
libsecret's service (`secret_service_disconnect`) when it changed; a call that
still fails with such an error is retried once with a new service.

## Testing

- `laufey_secret_store_dbus_test` (ctest, Linux): on a private bus, libsecret
  missing, no session bus, no provider, KWallet without the Secret Service, a
  provider that never answers, a locked item where no one can answer (refused at
  once for lookup, store and delete), and a real `gnome-keyring-daemon`: the
  round trips, a second daemon taking the name over (written to and read from),
  then locked without a prompter (at once) and with a prompter that never
  answers (after the timeout).
- `scripts/native-e2e-run.sh <backend> --secret-store`: every call answers
  within its timeout through the backend;
  `LAUFEY_E2E_EXPECT_SECRET=ok|unavailable` for a real session.
