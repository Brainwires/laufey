// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! `liblaufey_webauthn.so`: WebAuthn ceremonies with security keys on Linux,
//! over [libwebauthn](https://github.com/linux-credentials/libwebauthn), for
//! the CEF and WebView backends (`backend-common/src/passkey_linux.cc`), which
//! `dlopen()` it from their own directory. Its C ABI is
//! `backend-common/src/laufey_webauthn_abi.h`; nothing else is public.
//!
//! The backend validates the options first (laufey's strict parser, the
//! `passkeyRpIds` pin) and hands this library a canonical options document
//! with the RP ID. A ceremony runs on a thread of its own with a
//! current-thread Tokio runtime: it waits for a key (USB / HID; with several,
//! the one touched first), runs the CTAP2 (or U2F) operation, forwards
//! libwebauthn's user-verification updates (touch the key, enter the PIN)
//! through the callbacks, and ends with exactly one result document.
//! `clientDataJSON` is built by libwebauthn from the origin
//! `https://<rpId>`, in WebAuthn's member order (as laufey does on Windows).
//!
//! Nothing here logs: challenges, user handles, PINs and credentials never
//! reach stderr, and error messages name no values.

#[cfg(target_os = "linux")]
mod ceremony;
#[cfg(target_os = "linux")]
pub use ceremony::*;

#[cfg(all(target_os = "linux", any(test, feature = "virt-test")))]
pub mod virtual_key;
