// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The C ABI of liblaufey_webauthn.so (webauthn-linux/, Rust over
// libwebauthn), which passkey_linux.cc dlopen()s from the backend's own
// directory. Internal to backend-common; the library's src/ceremony.rs is
// the other side, and ABI 1 is what both speak.

#ifndef LAUFEY_WEBAUTHN_ABI_H_
#define LAUFEY_WEBAUTHN_ABI_H_

#include <stddef.h>
#include <stdint.h>

#define LAUFEY_WEBAUTHN_ABI_VERSION 1u
#define LAUFEY_WEBAUTHN_LIBRARY "liblaufey_webauthn.so"

// laufey_webauthn_capabilities: security keys over USB (HID) can be used.
#define LAUFEY_WEBAUTHN_CAP_SECURITY_KEYS (1u << 1)

// What the user is asked to do now (the status callback).
#define LAUFEY_WEBAUTHN_STATUS_INSERT_KEY 1u  // no key plugged in yet
#define LAUFEY_WEBAUTHN_STATUS_SELECT_KEY 2u  // several: touch the one to use
#define LAUFEY_WEBAUTHN_STATUS_TOUCH_KEY 3u   // touch the key
#define LAUFEY_WEBAUTHN_STATUS_PIN_RETRY 4u   // the PIN was wrong; asked again

#ifdef __cplusplus
extern "C" {
#endif

typedef struct laufey_webauthn_pin_reply laufey_webauthn_pin_reply_t;
typedef struct laufey_webauthn_handle laufey_webauthn_handle_t;

// Called on the ceremony's own thread.
typedef struct {
  void* user_data;
  // A LAUFEY_WEBAUTHN_STATUS_*. May be NULL.
  void (*status)(void* user_data, uint32_t status);
  // The key needs its PIN: attempts left in total (-1 unknown), and the
  // reply, answered exactly once with laufey_webauthn_pin_reply (NULL or ""
  // cancels). NULL: every PIN request is cancelled.
  void (*pin)(void* user_data, int32_t attempts_left,
              laufey_webauthn_pin_reply_t* reply);
  // The result document, exactly once, last:
  //   {"ok":true,"credentialId","clientDataJSON","attestationObject",
  //    "transports","attachment"}                         (registration)
  //   {"ok":true,"credentialId","clientDataJSON","authenticatorData",
  //    "signature","userHandle","attachment"}             (authentication)
  //   {"ok":false,"code","message"}
  // with binary members unpadded base64url.
  void (*done)(void* user_data, const char* result_json);
} laufey_webauthn_callbacks_t;

typedef uint32_t (*laufey_webauthn_abi_fn)(void);
typedef uint32_t (*laufey_webauthn_capabilities_fn)(char* reason,
                                                    size_t reason_len);
// `kind`: LAUFEY_PASSKEY_CREATE / _GET; `options_json`: WebAuthn JSON;
// `rp_id`: its (validated) RP ID; `timeout_ms`: the whole ceremony.
// NULL for a NULL argument or text that isn't UTF-8 (`done` never runs).
typedef laufey_webauthn_handle_t* (*laufey_webauthn_start_fn)(
    uint32_t kind, const char* options_json, const char* rp_id,
    uint32_t timeout_ms, const laufey_webauthn_callbacks_t* callbacks);
typedef void (*laufey_webauthn_cancel_fn)(laufey_webauthn_handle_t* handle);
typedef void (*laufey_webauthn_release_fn)(laufey_webauthn_handle_t* handle);
typedef void (*laufey_webauthn_pin_reply_fn)(laufey_webauthn_pin_reply_t* reply,
                                             const char* pin);
// Only in a library built with the `virt-test` feature (tests).
typedef void (*laufey_webauthn_test_use_virtual_key_fn)(const char* pin,
                                                        uint32_t flags);
typedef void (*laufey_webauthn_test_set_virtual_key_flags_fn)(uint32_t flags);

#ifdef __cplusplus
}
#endif

#endif  // LAUFEY_WEBAUTHN_ABI_H_
