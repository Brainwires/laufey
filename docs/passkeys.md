# Passkeys

A page served from a custom scheme (`app://`, `myapp://`) or a loopback origin
can't use the browser engine's WebAuthn for a relying party on the web: the RP
ID (`example.com`) has to match the page's origin. laufey runs the ceremony
through the OS platform authenticator instead — Touch ID / iCloud Keychain on
macOS, Windows Hello on Windows, and roaming security keys on both and on Linux
(through libwebauthn) — for the app's own trusted code to call.

```rust
let caps = laufey::passkey_capabilities();
if caps.platform_authenticator {
  // `options` is PublicKeyCredentialRequestOptions as JSON, binary members
  // base64url-encoded. Window 0: the focused window.
  let envelope: String = laufey::passkey_get(0, &options).await;
}
```

The wire format is deliberately the one of
[`@clerk/electron-passkeys`](https://github.com/clerk/javascript/tree/main/packages/electron-passkeys),
so its JavaScript side (`@clerk/electron/passkeys`) can run on top unchanged:
the options go in as WebAuthn JSON with unpadded base64url binary members, and
every request resolves with one JSON envelope.

## API

C ABI (API ≥ 37, `capi/include/laufey.h`):

```c
uint32_t (*passkey_capabilities)(void* backend_data);
void (*passkey_request)(void* backend_data, uint32_t window_id, uint32_t kind,
                        const char* options_json,
                        laufey_passkey_result_fn callback, void* user_data);

typedef void (*laufey_passkey_result_fn)(void* user_data,
                                         const char* result_json);
```

- `passkey_capabilities` returns `LAUFEY_PASSKEY_PLATFORM_AUTHENTICATOR` and/or
  `LAUFEY_PASSKEY_SECURITY_KEYS`. Any thread.
- `passkey_request` starts a ceremony: `kind` is `LAUFEY_PASSKEY_CREATE`
  (registration) or `LAUFEY_PASSKEY_GET` (authentication). `window_id` anchors
  the OS sheet / dialog; `0` means the focused window. Any thread.
- Both pointers are `NULL` on the Winit backend and on backends older than API
  37.

Rust (`laufey` crate): `passkey_capabilities() -> PasskeyCapabilities`
(`platform_authenticator`, `security_keys`), and
`passkey_create(window_id, options_json)` /
`passkey_get(window_id,
options_json)`, which return
`impl Future<Output = String>` resolving with the envelope. The request is made
when the function is called. A `NULL` backend pointer resolves with
`not_supported`.

### Options

Registration (`PublicKeyCredentialCreationOptions`):

```json
{
  "rp": { "id": "example.com", "name": "Example" },
  "user": { "id": "dXNlcl8x", "name": "ada@example.com", "displayName": "Ada" },
  "challenge": "-_8AAQIDBAUGBwgJCgsMDQ4P",
  "pubKeyCredParams": [{ "type": "public-key", "alg": -7 }],
  "timeout": 60000,
  "authenticatorSelection": {
    "authenticatorAttachment": "platform",
    "residentKey": "required",
    "userVerification": "required"
  },
  "attestation": "none",
  "excludeCredentials": [{ "type": "public-key", "id": "AQID-g" }]
}
```

Authentication (`PublicKeyCredentialRequestOptions`):

```json
{
  "challenge": "CQgHBgUEAwIBAP7__T4_QA",
  "rpId": "example.com",
  "timeout": 300000,
  "userVerification": "preferred",
  "allowCredentials": [{ "type": "public-key", "id": "AQID-g" }]
}
```

The backend parses them strictly before anything reaches the OS:

- at most `LAUFEY_PASSKEY_MAX_OPTIONS_BYTES` (64 KiB), valid UTF-8, one JSON
  object, no member given twice, nesting at most 32 deep;
- required: `rp.id` / `rpId`, `challenge` (non-empty), and for a registration
  `user` with a 1–64 byte `user.id`;
- binary members are canonical unpadded base64url (up to two trailing `=` are
  tolerated); credential ids are 1–1023 bytes; at most 256 entries in
  `excludeCredentials` / `allowCredentials` and 64 in `pubKeyCredParams`;
- the RP ID is a lowercase ASCII domain name (LDH labels, ≤ 253 characters, not
  an IP address; internationalized names in their punycode form);
- `timeout` is a non-negative integer, clamped to 1 s – 10 min;
- enumeration values a version doesn't know (an `attestation` of `"bogus"`) and
  unknown members (`extensions`, `hints`) are ignored, as WebAuthn says.

A missing or malformed RP ID is `invalid_rp`; any other problem is `unknown`
with a message naming the member (never its value).

### Pinning the relying parties

An app can list the RP IDs it uses in its [launch file](launch-config.md):

```json
{ "appId": "com.example.app", "passkeyRpIds": ["example.com"] }
```

With `passkeyRpIds` present, a request whose RP ID is not in the list (compared
without regard to case) is refused with `invalid_rp` before it takes the
one-ceremony slot or shows any OS UI; an empty list refuses every request.
Without the key every RP ID that passes the parser goes to the OS, as before.
The key has no environment variable: the list ships with the installed app and
nothing at run time can widen it. Ship it on Windows in particular, where the OS
does not tie RP IDs to the app.

### Result

```json
{
  "ok": true,
  "credential": {
    "id": "AQID-g",
    "rawId": "AQID-g",
    "type": "public-key",
    "authenticatorAttachment": "platform",
    "response": {
      "clientDataJSON": "…",
      "attestationObject": "…",
      "transports": ["internal", "hybrid"]
    }
  }
}
```

An authentication's `response` holds `clientDataJSON`, `authenticatorData`,
`signature` and, for a discoverable credential, `userHandle`. An
`authenticatorAttachment` the OS doesn't report is left out.

```json
{ "ok": false, "error": { "code": "cancelled", "message": "…" } }
```

| `code`          | Meaning                                                                                     |
| --------------- | ------------------------------------------------------------------------------------------- |
| `cancelled`     | The user dismissed the OS UI, or the anchor window closed.                                  |
| `invalid_rp`    | The RP ID is malformed, not in `passkeyRpIds`, or OS-refused.                               |
| `not_supported` | No platform API (Winit, macOS < 12, Windows < 1903, Linux without `liblaufey_webauthn.so`). |
| `timeout`       | The options' `timeout` passed.                                                              |
| `unknown`       | Anything else: invalid options, busy, an OS error (its message)                             |

### Threading and lifetime

- The callback is invoked **exactly once** per request, on any thread:
  synchronously on the calling thread (before `passkey_request` returns) when
  the request is refused up front — invalid options, another request in
  progress, no platform API; on the main thread on macOS; on the ceremony's
  worker thread on Windows; on an internal timer thread for a timeout; on the UI
  thread when the anchor window closes. Don't block in it; copy the string.
- **One ceremony at a time per app.** A request made while another is in
  progress is answered `unknown` / "a passkey request is already in progress".
  The slot is freed before the result is delivered, so the next request can
  start from the callback. After a timeout or a closed window the result is
  delivered at once, but the slot stays taken until the OS has actually ended
  the operation.

## macOS

macOS 12+, through AuthenticationServices: an `ASAuthorizationController` with
an `ASAuthorizationPlatformPublicKeyCredentialProvider` request (iCloud
Keychain, Touch ID, a phone over hybrid) and an
`ASAuthorizationSecurityKeyPublicKeyCredentialProvider` request (USB / NFC / BLE
keys) in the same sheet. A registration leaves out the provider its
`authenticatorAttachment` excludes; an authentication leaves out a provider none
of the `allowCredentials` transports can reach (when every entry names its
transports). The sheet is attached to the NSWindow of `window_id` (the key /
main / first visible window for `0`). All of it runs on the main thread.

**The OS builds `clientDataJSON` itself** from the RP ID, with origin
`https://<rpId>`; the server sees the same as from Safari on that domain.

**The app must be associated with the RP's domain.** A process that is not a
browser may only use an RP ID that its code signature allows:

1. the `com.apple.developer.associated-domains` entitlement with
   `webcredentials:<rp-id>` (e.g. `webcredentials:clerk.example.com`),
2. a provisioning profile that grants it (associated domains is a restricted
   entitlement: Developer ID builds need a profile from the developer portal,
   with the app's identifier configured for Associated Domains),
3. and on the domain, `https://<rp-id>/.well-known/apple-app-site-association`
   listing the app:

   ```json
   { "webcredentials": { "apps": ["ABCDE12345.com.example.app"] } }
   ```

   (`<Team ID>.<bundle identifier>`; Apple's CDN caches the file, append
   `?mode=developer` to the entitlement value while iterating).

Without them the OS fails the request without showing anything, and laufey
answers `invalid_rp` — for "Application with identifier … is not associated with
domain …" and for "The calling process does not have an application identifier"
(an unsigned or ad-hoc signed build).

Errors: `ASAuthorizationErrorCanceled` → `cancelled`; `Failed` carrying the
association texts above → `invalid_rp`; `NotHandled` / `NotInteractive` →
`not_supported`; a "timed out" failure → `timeout`; anything else → `unknown`
with the error's description.

AuthenticationServices has no timeout of its own, so laufey runs the options'
`timeout` (when given; none otherwise) and cancels the controller when it
passes. `-[ASAuthorizationController cancel]` exists from macOS 13; on macOS 12
the sheet stays up until the user dismisses it (the `timeout` result is still
delivered on time, and the next request is refused until then). A platform
registration doesn't forward the `attestation` preference (iCloud Keychain fails
such requests; browsers downgrade them to `none` too), and `excludeCredentials`
reaches the platform provider from macOS 13.5.

## Windows

Windows 10 1903+, through the Windows WebAuthn API: `webauthn.dll` is loaded at
runtime from System32 (the host starts without it), and needs API version ≥ 1.
`WebAuthNAuthenticatorMakeCredential` / `GetAssertion` block while the system
dialog is up, so each ceremony runs on its own worker thread with the HWND of
`window_id` as the dialog's owner (for `0`: the foreground window when it
belongs to the app, else the app's first visible window). Windows accepts any RP
ID; no entitlement or domain file is involved. Capabilities: security keys
whenever the API is there, the platform authenticator when
`WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable` says Windows Hello is
set up.

**laufey builds `clientDataJSON`** (on Windows the caller does, and the OS
hashes it):

```json
{
  "type": "webauthn.get",
  "challenge": "<challenge, base64url>",
  "origin": "https://<rpId>",
  "crossOrigin": false
}
```

in the member order of WebAuthn's limited verification algorithm. The origin is
`https://<rpId>` — what `@clerk/electron-passkeys` sends, and what an RP that
checks the origin against its own domain expects; the app's real origin (a
custom scheme) would be rejected by such servers.

The timeout is the options' `timeout`, 60 s when none is given (as
`@clerk/electron-passkeys`). laufey cancels the operation itself when it passes
(`WebAuthNCancelCurrentOperation` with the request's cancellation id) and when
the owner window is destroyed; the OS gets 10 s more as a backstop, so the
result is always `timeout` / `cancelled` from laufey, not a race with the OS.
The OS answers a cancel that arrives before the call has registered its
cancellation id with `S_OK` and drops it, and where nobody answers the dialog
the call runs past its own timeout too, so laufey repeats the cancel every 250
ms until the call returns (for at most 60 s); one request runs at a time, so the
next one waits for that.

Errors: `NTE_USER_CANCELLED` and `ERROR_CANCELLED` → `cancelled`;
`ERROR_TIMEOUT` → `timeout`; `NTE_NOT_SUPPORTED` → `not_supported`; otherwise by
the DOMException name `WebAuthNGetErrorName` gives the HRESULT:
`NotAllowedError` → `cancelled`, `SecurityError` → `invalid_rp`,
`NotSupportedError` / `ConstraintError` → `not_supported`, anything else →
`unknown` with the system message. A bad RP ID is caught by the parser before
the OS sees it.

## Linux

Linux has no platform authenticator API, so there laufey runs the roaming half:
**security keys over USB** (FIDO2 / CTAP2, and U2F keys), through
[libwebauthn](https://github.com/linux-credentials/libwebauthn), on the CEF and
WebKitGTK backends. `passkey_capabilities` reports
`LAUFEY_PASSKEY_SECURITY_KEYS` (never `PLATFORM_AUTHENTICATOR`), with or without
a key plugged in.

**The library.** libwebauthn lives in `liblaufey_webauthn.so` (`webauthn-linux/`
in this repository, Rust), a shared library of its own that the backend
`dlopen()`s from **its own directory** (the directory of the host executable,
`/proc/self/exe`) the first time passkeys are asked about. No environment
variable names another file. `make webview` / `make cef` build it and copy it
beside the host when the libudev and libdbus-1 development files are there
(`libudev-dev libdbus-1-dev` on Debian / Ubuntu, `systemd-devel dbus-devel` on
Fedora); without them the build says so and skips it. A backend without the
library, or whose library can't list HID devices, reports no capabilities and
answers `not_supported` with the reason, which `platform_features` also gives
(`passkeys`, `passkeysReason`). At run time the library needs `libudev.so.1` and
`libdbus-1.so.3`, present on every desktop. The backends link none of it (no
Tokio, D-Bus or udev in the host process until a passkey is used), and
libwebauthn (LGPL-2.1-or-later) stays a separately replaceable library: an app
that ships `liblaufey_webauthn.so` ships LGPL code, as a library the user can
rebuild from `webauthn-linux/` and replace.

**Access to the key.** Keys are read through `/dev/hidraw*`. systemd's udev
rules give the logged-in user access to FIDO devices (the `uaccess` tag, from
systemd 244's FIDO detection); on a system without them, add a udev rule for the
key or the request finds no key.

**The ceremony.** The backend parses and pins the options as everywhere
([Options](#options)), then hands the library a canonical options document built
from the parsed values (never the caller's text) with the RP ID and the timeout.
The library's own thread waits for a key (several plugged in: each blinks and
the first one touched is used), runs MakeCredential or GetAssertion (with
libwebauthn's preflight of the exclude / allow list), and asks for the PIN when
the key needs it. **libwebauthn builds `clientDataJSON`** from the origin
`https://<rpId>`, in WebAuthn's member order, byte for byte what laufey builds
on Windows. The registration's `authenticatorAttachment` is `"cross-platform"`
and its `transports` what the key reports (`"usb"`). With several discoverable
credentials for the RP on one key, the first the key returns is used (there is
no account picker yet).

**The UI** is laufey's own, in GTK, on the GTK thread: while the key is wanted a
dialog says "Insert your security key", "Touch the security key you want to use"
or "Touch your security key" (with the RP ID) and has a Cancel button
(`cancelled`); when the key needs its PIN a dialog asks for it (a masked entry;
after a wrong PIN it says so, and with three attempts left or fewer it says how
many). The PIN goes to the key through CTAP's PIN protocol and is not kept: the
entry is cleared. On WebKitGTK the dialogs are transient for the request's
window; on CEF they have no parent (Chromium's window is not a GTK one). A PIN
is never set from here: a key with no PIN that must verify the user answers
`not_supported`.

**Refused up front**: a registration whose `authenticatorAttachment` is
`"platform"` (`not_supported`: there is no platform authenticator, and a
security key is not what the RP asked for; `@clerk/electron-passkeys` asks for
one, so its registrations are refused on Linux), and an authentication whose
every allowed credential names transports none of which is `"usb"` (a passkey on
a phone or in a platform authenticator can't be reached).

The timeout is the options' `timeout`, 60 s when none is given. laufey's own
timer ends the request (`timeout`) and cancels the library's operation (the key
gets `CTAPHID_CANCEL`); the library runs to the timeout plus 5 s as a backstop.
Closing the request's window cancels it (`cancelled`).

Errors: the user cancelling (the dialog, the PIN entry, or on the key) and a key
that holds no credential for the site → `cancelled`; a key not touched in time →
`timeout`; a key without a PIN where user verification is required, an algorithm
or option the key doesn't support → `not_supported`; a credential the exclude
list names already on the key, a wrong PIN, a blocked PIN, a full key and
everything else → `unknown` with a message that names no value.

caBLE / hybrid (a phone over Bluetooth and a tunnel server, which libwebauthn
supports) and NFC are not offered: they need a QR code and Bluetooth (or a
reader) and are a later step.

## Winit

The Winit backend leaves both pointers `NULL`.

## Security notes

- **The options are untrusted input** to the backend, and the result is a
  credential for the RP: only the app's own trusted code should be able to start
  a ceremony. Never expose `passkey_request` to arbitrary web content — this
  path skips the browser's origin check, and on Windows nothing ties the RP ID
  to the app at all, so a page that can reach it could ask for an assertion for
  any site.
- On macOS the OS enforces the RP ↔ app association (entitlement + AASA); on
  Windows it doesn't. List the app's RP IDs in `passkeyRpIds`
  ([above](#pinning-the-relying-parties)) so the backend refuses any other, and
  check the RP ID against the ones the app expects before calling.
- Nothing in this path logs: challenges, user handles and credentials never
  reach stderr, and error messages name members, not their values.
- One ceremony at a time keeps a second request from hijacking the OS UI the
  user is looking at.

## Testing

`backend-common/tests/passkey_test.cc` (ctest `laufey_passkey_test`) covers the
parser against byte-for-byte output of `@clerk/electron`'s serializers,
base64url / UTF-8 / RP ID rules, the `passkeyRpIds` pin, the envelopes, and the
ceremony lifecycle (one at a time, exactly-once delivery, timeout, abort, window
close, concurrent requests). `passkey_mac_test.mm` covers the macOS error
mapping. On Linux, `webauthn-linux`'s own tests (`cargo test`) run whole
ceremonies through its C ABI against a virtual CTAP2.1 security key behind
libwebauthn's virtual HID device (`src/virtual_key.rs`: ES256 credentials,
resident keys, the exclude and allow lists, PIN protocol 1, refusals on the
key): registration then authentication with the signature checked against the
attested key, a discoverable credential, a wrong PIN then the right one, a
cancelled PIN entry, no key plugged in (cancel, timeout, plugged in later), and
options libwebauthn refuses. `passkey_linux_test.cc` (ctest
`laufey_passkey_linux_test`) checks the canonical options documents, the
envelopes and the missing library, and, given the library built with its
`virt-test` feature (`LAUFEY_TEST_WEBAUTHN_LIBRARY`), whole ceremonies through
`PasskeyBegin` / `PasskeyStartLinux` with the UI hooked: the PIN, the window
closing while a key is awaited, the timeout, and the requests refused up front.
The native e2e battery checks the capabilities per OS (Linux: security keys
only, required where CI built the library), that macOS / Windows / Linux refuse
malformed options, and that a real request is answered exactly once (on Linux,
the "insert your security key" dialog until laufey's timeout).

A real ceremony needs a person at the machine (and on macOS a signed, associated
build): run one against a test RP from a build of the app with the entitlement,
register a passkey with Touch ID / Windows Hello, sign in with it, cancel the
sheet (→ `cancelled`), let it time out (→ `timeout`), and close the window while
it is up (→ `cancelled`). On Linux the same with a real USB security key (no
virtual key exercises the HID transport itself, a key's own PIN rules or several
keys plugged in at once): register, sign in with and without a PIN, enter a
wrong PIN, cancel the dialog, and pull the key mid-request.
