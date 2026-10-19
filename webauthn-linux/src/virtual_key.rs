// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! A virtual security key for tests: a CTAP2.1 authenticator behind
//! libwebauthn's virtual HID device (its `virt` feature), so a ceremony runs
//! the real CTAPHID framing, CBOR, PIN protocol and signatures with no
//! hardware. It speaks CTAPHID INIT / CBOR / CANCEL / WINK and, over CBOR,
//! authenticatorGetInfo, MakeCredential (ES256, "none" attestation,
//! excludeList, resident keys), GetAssertion (allowList or discoverable,
//! silent `up: false` probes), ClientPIN (PIN/UV auth protocol 1: retries,
//! key agreement, getPinToken and getPinUvAuthTokenUsingPinWithPermissions)
//! and authenticatorSelection. Behaviours for tests: a PIN, refusing every
//! operation that needs a touch (the user cancels on the key), and no key
//! plugged in at all.
//!
//! Compiled for `cargo test` and the `virt-test` feature only, never into a
//! shipped library.

use std::ffi::{c_char, CStr};
use std::sync::{Arc, Mutex};

use aes::Aes256;
use cbc::cipher::block_padding::NoPadding;
use cbc::cipher::{BlockDecryptMut, BlockEncryptMut, KeyIvInit};
use ciborium::value::Value;
use hmac::{Hmac, Mac};
use libwebauthn::transport::hid::framing::{HidCommand, HidMessage};
use libwebauthn::transport::hid::{virtual_device, HidDevice, HidPipeBackend};
use p256::ecdsa::signature::Signer;
use p256::ecdsa::{Signature, SigningKey};
use p256::elliptic_curve::sec1::{FromEncodedPoint, ToEncodedPoint};
use p256::{EncodedPoint, PublicKey, SecretKey};
use rand::rngs::OsRng;
use rand::RngCore;
use sha2::{Digest, Sha256};

/// Behaviour flags (`laufey_webauthn_test_use_virtual_key`).
pub const FLAG_DENY: u32 = 1 << 0;
pub const FLAG_ABSENT: u32 = 1 << 1;
/// Removes the virtual key: ceremonies use real HID devices again.
pub const UNINSTALL: u32 = u32::MAX;

const AAGUID: [u8; 16] = *b"laufey-test-key!";
const MAX_PIN_RETRIES: i64 = 8;

// CTAP2 status codes.
const OK: u8 = 0x00;
const ERR_INVALID_COMMAND: u8 = 0x01;
const ERR_INVALID_PARAMETER: u8 = 0x02;
const ERR_MISSING_PARAMETER: u8 = 0x14;
const ERR_CREDENTIAL_EXCLUDED: u8 = 0x19;
const ERR_UNSUPPORTED_ALGORITHM: u8 = 0x26;
const ERR_OPERATION_DENIED: u8 = 0x27;
const ERR_NO_CREDENTIALS: u8 = 0x2E;
const ERR_PIN_INVALID: u8 = 0x31;
const ERR_PIN_BLOCKED: u8 = 0x32;
const ERR_PIN_AUTH_INVALID: u8 = 0x33;
const ERR_PIN_NOT_SET: u8 = 0x35;
const ERR_PIN_REQUIRED: u8 = 0x36;
const ERR_INVALID_SUBCOMMAND: u8 = 0x3E;

struct Credential {
  id: Vec<u8>,
  rp_id_hash: [u8; 32],
  user_id: Option<Vec<u8>>,
  key: SigningKey,
  sign_count: u32,
}

/// The key's state, shared by every ceremony while it is installed.
pub struct KeyState {
  pub flags: u32,
  pin: Option<String>,
  pin_retries: i64,
  agreement: SecretKey,
  pin_token: Option<[u8; 32]>,
  credentials: Vec<Credential>,
  /// Requests seen, by CTAP2 command byte (tests read it).
  pub commands: Vec<u8>,
}

impl KeyState {
  fn new(pin: Option<String>, flags: u32) -> Self {
    Self {
      flags,
      pin,
      pin_retries: MAX_PIN_RETRIES,
      agreement: SecretKey::random(&mut OsRng),
      pin_token: None,
      credentials: Vec::new(),
      commands: Vec::new(),
    }
  }
}

static CURRENT: Mutex<Option<Arc<Mutex<KeyState>>>> = Mutex::new(None);

/// Installs a fresh virtual key (`pin`, `flags`), or (`UNINSTALL`) removes it.
pub fn install(pin: Option<&str>, flags: u32) {
  let mut current = CURRENT.lock().unwrap();
  *current = if flags == UNINSTALL {
    None
  } else {
    Some(Arc::new(Mutex::new(KeyState::new(
      pin.map(str::to_string),
      flags,
    ))))
  };
}

/// Changes the installed key's flags, keeping its credentials (a key
/// plugged in later, say).
pub fn set_flags(flags: u32) {
  if let Some(state) = CURRENT.lock().unwrap().as_ref() {
    state.lock().unwrap().flags = flags;
  }
}

/// The installed key's state (tests).
pub fn state() -> Option<Arc<Mutex<KeyState>>> {
  CURRENT.lock().unwrap().clone()
}

/// The devices a ceremony sees while a virtual key is installed: the key, or
/// none (`FLAG_ABSENT`). None when no virtual key is installed.
pub fn devices() -> Option<Vec<HidDevice>> {
  let state = CURRENT.lock().unwrap().clone()?;
  if state.lock().unwrap().flags & FLAG_ABSENT != 0 {
    return Some(Vec::new());
  }
  Some(vec![virtual_device(Pipe {
    state,
    replies: Vec::new(),
  })])
}

/// Test-only C entry point: installs a virtual key with `pin` (null: none)
/// and `flags`, or removes it (`flags` = `u32::MAX`). Only in a library built
/// with the `virt-test` feature.
///
/// # Safety
/// `pin` is null or NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn laufey_webauthn_test_use_virtual_key(
  pin: *const c_char,
  flags: u32,
) {
  let pin = if pin.is_null() {
    None
  } else {
    CStr::from_ptr(pin).to_str().ok()
  };
  install(pin, flags);
}

/// Test-only: the installed key's flags changed, its credentials kept.
#[no_mangle]
pub extern "C" fn laufey_webauthn_test_set_virtual_key_flags(flags: u32) {
  set_flags(flags);
}

// --- CTAPHID ------------------------------------------------------------------

struct Pipe {
  state: Arc<Mutex<KeyState>>,
  replies: Vec<HidMessage>,
}

impl std::fmt::Debug for Pipe {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    f.write_str("laufey virtual key")
  }
}

const CHANNEL_ID: u32 = 0x4c41_5546;

impl HidPipeBackend for Pipe {
  fn send(&mut self, msg: &HidMessage) {
    match msg.cmd {
      HidCommand::Init => {
        let mut payload = msg.payload.clone();
        payload.truncate(8);
        payload.extend_from_slice(&CHANNEL_ID.to_be_bytes());
        // CTAPHID protocol 2, version 1.0.0, WINK | CBOR | NMSG (no U2F).
        payload.extend_from_slice(&[2, 1, 0, 0, 0x01 | 0x04 | 0x08]);
        self
          .replies
          .push(HidMessage::new(msg.cid, HidCommand::Init, &payload));
      }
      HidCommand::Cbor => {
        let answer = self.state.lock().unwrap().cbor(&msg.payload);
        self
          .replies
          .push(HidMessage::new(msg.cid, HidCommand::Cbor, &answer));
      }
      HidCommand::Wink | HidCommand::Ping => {
        self
          .replies
          .push(HidMessage::new(msg.cid, msg.cmd, &msg.payload));
      }
      // No answer to a cancel (nothing is ever pending here).
      HidCommand::Cancel => {}
      _ => {
        self
          .replies
          .push(HidMessage::new(msg.cid, HidCommand::Error, &[0x01]));
      }
    }
  }

  fn recv(&mut self) -> HidMessage {
    if self.replies.is_empty() {
      return HidMessage::new(CHANNEL_ID, HidCommand::Error, &[0x7f]);
    }
    self.replies.remove(0)
  }
}

// --- CBOR ---------------------------------------------------------------------

fn int(i: i64) -> Value {
  Value::Integer(i.into())
}

fn text(s: &str) -> Value {
  Value::Text(s.to_string())
}

fn map(entries: Vec<(Value, Value)>) -> Value {
  Value::Map(entries)
}

fn get(m: &[(Value, Value)], key: i64) -> Option<&Value> {
  m.iter()
    .find(|(k, _)| k.as_integer().map(i128::from) == Some(key as i128))
    .map(|(_, v)| v)
}

fn get_text<'a>(m: &'a [(Value, Value)], key: &str) -> Option<&'a Value> {
  m.iter()
    .find(|(k, _)| k.as_text() == Some(key))
    .map(|(_, v)| v)
}

fn as_bytes(v: Option<&Value>) -> Option<Vec<u8>> {
  v.and_then(Value::as_bytes).cloned()
}

fn as_int(v: Option<&Value>) -> Option<i64> {
  v.and_then(Value::as_integer)
    .and_then(|i| i64::try_from(i).ok())
}

fn option(m: &[(Value, Value)], key: i64, name: &str) -> Option<bool> {
  get(m, key)
    .and_then(Value::as_map)
    .and_then(|o| get_text(o, name))
    .and_then(Value::as_bool)
}

fn encode(status: u8, body: Option<Value>) -> Vec<u8> {
  let mut out = vec![status];
  if let Some(body) = body {
    ciborium::ser::into_writer(&body, &mut out).unwrap();
  }
  out
}

fn cose_key(point: &EncodedPoint, alg: i64) -> Value {
  map(vec![
    (int(1), int(2)),
    (int(3), int(alg)),
    (int(-1), int(1)),
    (int(-2), Value::Bytes(point.x().unwrap().to_vec())),
    (int(-3), Value::Bytes(point.y().unwrap().to_vec())),
  ])
}

fn sha256(data: &[u8]) -> [u8; 32] {
  Sha256::digest(data).into()
}

fn hmac16(key: &[u8], data: &[u8]) -> Vec<u8> {
  let mut mac = <Hmac<Sha256> as Mac>::new_from_slice(key).unwrap();
  mac.update(data);
  mac.finalize().into_bytes()[..16].to_vec()
}

impl KeyState {
  fn cbor(&mut self, payload: &[u8]) -> Vec<u8> {
    let Some((&command, rest)) = payload.split_first() else {
      return encode(ERR_INVALID_COMMAND, None);
    };
    self.commands.push(command);
    let params: Vec<(Value, Value)> = if rest.is_empty() {
      Vec::new()
    } else {
      match ciborium::de::from_reader::<Value, _>(rest) {
        Ok(Value::Map(m)) => m,
        _ => return encode(ERR_INVALID_PARAMETER, None),
      }
    };
    match command {
      0x04 => self.get_info(),
      0x01 => self.make_credential(&params),
      0x02 => self.get_assertion(&params),
      0x06 => self.client_pin(&params),
      0x0B => encode(OK, None), // authenticatorSelection: touched
      _ => encode(ERR_INVALID_COMMAND, None),
    }
  }

  fn get_info(&self) -> Vec<u8> {
    let options = map(vec![
      (text("rk"), Value::Bool(true)),
      (text("up"), Value::Bool(true)),
      (text("clientPin"), Value::Bool(self.pin.is_some())),
      (text("pinUvAuthToken"), Value::Bool(true)),
      (text("makeCredUvNotRqd"), Value::Bool(true)),
    ]);
    encode(
      OK,
      Some(map(vec![
        (
          int(1),
          Value::Array(vec![text("FIDO_2_0"), text("FIDO_2_1")]),
        ),
        (int(3), Value::Bytes(AAGUID.to_vec())),
        (int(4), options),
        (int(5), int(1200)),
        (int(6), Value::Array(vec![int(1)])),
        (int(7), int(8)),
        (int(8), int(128)),
        (int(9), Value::Array(vec![text("usb")])),
        (
          int(10),
          Value::Array(vec![map(vec![
            (text("alg"), int(-7)),
            (text("type"), text("public-key")),
          ])]),
        ),
      ])),
    )
  }

  /// Checks a request's pinUvAuthParam against the current token: Ok(true)
  /// verified, Ok(false) none given, Err(status) wrong.
  fn verify_pin_auth(
    &self,
    params: &[(Value, Value)],
    auth_key: i64,
    client_data_hash: &[u8],
  ) -> Result<bool, u8> {
    let Some(auth) = as_bytes(get(params, auth_key)) else {
      return Ok(false);
    };
    let Some(token) = self.pin_token else {
      return Err(ERR_PIN_AUTH_INVALID);
    };
    if auth != hmac16(&token, client_data_hash) {
      return Err(ERR_PIN_AUTH_INVALID);
    }
    Ok(true)
  }

  fn make_credential(&mut self, params: &[(Value, Value)]) -> Vec<u8> {
    let (Some(client_data_hash), Some(rp), Some(user)) = (
      as_bytes(get(params, 1)),
      get(params, 2).and_then(Value::as_map),
      get(params, 3).and_then(Value::as_map),
    ) else {
      return encode(ERR_MISSING_PARAMETER, None);
    };
    let Some(rp_id) = get_text(rp, "id").and_then(Value::as_text) else {
      return encode(ERR_MISSING_PARAMETER, None);
    };
    let rp_id_hash = sha256(rp_id.as_bytes());
    let es256 = get(params, 4)
      .and_then(Value::as_array)
      .is_some_and(|algs| {
        algs.iter().any(|a| {
          a.as_map()
            .and_then(|m| get_text(m, "alg"))
            .and_then(|v| v.as_integer())
            .map(i128::from)
            == Some(-7)
        })
      });
    if !es256 {
      return encode(ERR_UNSUPPORTED_ALGORITHM, None);
    }
    let uv = match self.verify_pin_auth(params, 8, &client_data_hash) {
      Ok(v) => v,
      Err(status) => return encode(status, None),
    };
    let rk = option(params, 7, "rk").unwrap_or(false);
    if !uv
      && self.pin.is_some()
      && (rk || option(params, 7, "uv") == Some(true))
    {
      return encode(ERR_PIN_REQUIRED, None);
    }
    if !uv && option(params, 7, "uv") == Some(true) && self.pin.is_none() {
      return encode(ERR_PIN_NOT_SET, None);
    }
    if let Some(exclude) = get(params, 5).and_then(Value::as_array) {
      for d in exclude {
        let id = d
          .as_map()
          .and_then(|m| get_text(m, "id"))
          .and_then(Value::as_bytes);
        if let Some(id) = id {
          if self
            .credentials
            .iter()
            .any(|c| &c.id == id && c.rp_id_hash == rp_id_hash)
          {
            return encode(ERR_CREDENTIAL_EXCLUDED, None);
          }
        }
      }
    }
    if self.flags & FLAG_DENY != 0 {
      return encode(ERR_OPERATION_DENIED, None);
    }
    let key = SigningKey::random(&mut OsRng);
    let mut id = vec![0u8; 32];
    OsRng.fill_bytes(&mut id);
    let point = key.verifying_key().to_encoded_point(false);
    let mut auth_data = rp_id_hash.to_vec();
    auth_data.push(0x01 | if uv { 0x04 } else { 0 } | 0x40);
    auth_data.extend_from_slice(&1u32.to_be_bytes());
    auth_data.extend_from_slice(&AAGUID);
    auth_data.extend_from_slice(&(id.len() as u16).to_be_bytes());
    auth_data.extend_from_slice(&id);
    ciborium::ser::into_writer(&cose_key(&point, -7), &mut auth_data).unwrap();
    let user_id = get_text(user, "id").and_then(Value::as_bytes).cloned();
    self.credentials.push(Credential {
      id,
      rp_id_hash,
      user_id: if rk { user_id } else { None },
      key,
      sign_count: 1,
    });
    encode(
      OK,
      Some(map(vec![
        (int(1), text("none")),
        (int(2), Value::Bytes(auth_data)),
        (int(3), map(vec![])),
      ])),
    )
  }

  fn get_assertion(&mut self, params: &[(Value, Value)]) -> Vec<u8> {
    let (Some(rp_id), Some(client_data_hash)) = (
      get(params, 1).and_then(Value::as_text).map(str::to_string),
      as_bytes(get(params, 2)),
    ) else {
      return encode(ERR_MISSING_PARAMETER, None);
    };
    let rp_id_hash = sha256(rp_id.as_bytes());
    let allow: Vec<Vec<u8>> = get(params, 3)
      .and_then(Value::as_array)
      .map(|list| {
        list
          .iter()
          .filter_map(|d| {
            d.as_map()
              .and_then(|m| get_text(m, "id"))
              .and_then(Value::as_bytes)
              .cloned()
          })
          .collect()
      })
      .unwrap_or_default();
    let up = option(params, 5, "up").unwrap_or(true);
    let uv = match self.verify_pin_auth(params, 6, &client_data_hash) {
      Ok(v) => v,
      Err(status) => return encode(status, None),
    };
    let matching: Vec<usize> = self
      .credentials
      .iter()
      .enumerate()
      .filter(|(_, c)| {
        c.rp_id_hash == rp_id_hash
          && if allow.is_empty() {
            c.user_id.is_some()
          } else {
            allow.contains(&c.id)
          }
      })
      .map(|(i, _)| i)
      .collect();
    let Some(&first) = matching.first() else {
      return encode(ERR_NO_CREDENTIALS, None);
    };
    if up && self.flags & FLAG_DENY != 0 {
      return encode(ERR_OPERATION_DENIED, None);
    }
    let count = matching.len();
    let cred = &mut self.credentials[first];
    if up {
      cred.sign_count += 1;
    }
    let mut auth_data = rp_id_hash.to_vec();
    auth_data.push(if up { 0x01 } else { 0 } | if uv { 0x04 } else { 0 });
    auth_data.extend_from_slice(&cred.sign_count.to_be_bytes());
    let mut signed = auth_data.clone();
    signed.extend_from_slice(&client_data_hash);
    let signature: Signature = cred.key.sign(&signed);
    let mut response = vec![
      (
        int(1),
        map(vec![
          (text("id"), Value::Bytes(cred.id.clone())),
          (text("type"), text("public-key")),
        ]),
      ),
      (int(2), Value::Bytes(auth_data)),
      (int(3), Value::Bytes(signature.to_der().as_bytes().to_vec())),
    ];
    if let Some(user_id) = &cred.user_id {
      response.push((
        int(4),
        map(vec![(text("id"), Value::Bytes(user_id.clone()))]),
      ));
    }
    if allow.is_empty() && count > 1 {
      response.push((int(5), int(count as i64)));
    }
    encode(OK, Some(map(response)))
  }

  fn shared_secret(&self, params: &[(Value, Value)]) -> Option<[u8; 32]> {
    let key = get(params, 3)?.as_map()?;
    let x = as_bytes(get(key, -2))?;
    let y = as_bytes(get(key, -3))?;
    let point = EncodedPoint::from_affine_coordinates(
      x.as_slice().into(),
      y.as_slice().into(),
      false,
    );
    let public =
      Option::<PublicKey>::from(PublicKey::from_encoded_point(&point))?;
    let shared = p256::ecdh::diffie_hellman(
      self.agreement.to_nonzero_scalar(),
      public.as_affine(),
    );
    Some(sha256(shared.raw_secret_bytes()))
  }

  fn client_pin(&mut self, params: &[(Value, Value)]) -> Vec<u8> {
    match as_int(get(params, 2)) {
      // getPINRetries
      Some(0x01) => {
        encode(OK, Some(map(vec![(int(3), int(self.pin_retries))])))
      }
      // getKeyAgreement
      Some(0x02) => {
        let point = self.agreement.public_key().to_encoded_point(false);
        encode(OK, Some(map(vec![(int(1), cose_key(&point, -25))])))
      }
      // getPinToken / getPinUvAuthTokenUsingPinWithPermissions
      Some(0x05) | Some(0x09) => {
        let Some(pin) = self.pin.clone() else {
          return encode(ERR_PIN_NOT_SET, None);
        };
        if self.pin_retries <= 0 {
          return encode(ERR_PIN_BLOCKED, None);
        }
        let (Some(shared), Some(mut pin_hash_enc)) =
          (self.shared_secret(params), as_bytes(get(params, 6)))
        else {
          return encode(ERR_MISSING_PARAMETER, None);
        };
        if pin_hash_enc.len() != 16 {
          return encode(ERR_PIN_AUTH_INVALID, None);
        }
        let decrypted =
          cbc::Decryptor::<Aes256>::new(&shared.into(), &[0u8; 16].into())
            .decrypt_padded_mut::<NoPadding>(&mut pin_hash_enc)
            .map(|d| d.to_vec())
            .unwrap_or_default();
        // A new key agreement after every attempt, as a real key does.
        self.agreement = SecretKey::random(&mut OsRng);
        if decrypted != sha256(pin.as_bytes())[..16] {
          self.pin_retries -= 1;
          return encode(
            if self.pin_retries <= 0 {
              ERR_PIN_BLOCKED
            } else {
              ERR_PIN_INVALID
            },
            None,
          );
        }
        self.pin_retries = MAX_PIN_RETRIES;
        let mut token = [0u8; 32];
        OsRng.fill_bytes(&mut token);
        self.pin_token = Some(token);
        let mut encrypted = token;
        cbc::Encryptor::<Aes256>::new(&shared.into(), &[0u8; 16].into())
          .encrypt_padded_mut::<NoPadding>(&mut encrypted, 32)
          .unwrap();
        encode(
          OK,
          Some(map(vec![(int(2), Value::Bytes(encrypted.to_vec()))])),
        )
      }
      _ => encode(ERR_INVALID_SUBCOMMAND, None),
    }
  }
}
