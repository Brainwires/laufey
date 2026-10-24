// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Ceremonies end to end through the C ABI, against the virtual key
//! (`virtual_key.rs`) behind libwebauthn's virtual HID device: registration
//! and authentication (signatures checked with the attested public key),
//! discoverable credentials, the PIN (a wrong one, then the right one; a
//! cancelled entry), the exclude list, a refusal on the key, no key plugged
//! in (cancel, timeout, plugged in later), unknown credentials, options
//! libwebauthn can't parse, and the capability probe.

use std::collections::VecDeque;
use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::{Condvar, Mutex, MutexGuard};
use std::time::{Duration, Instant};

use ciborium::value::Value as Cbor;
use p256::ecdsa::signature::Verifier;
use p256::ecdsa::{Signature, VerifyingKey};
use p256::Sec1Point;
use serde_json::Value;
use sha2::{Digest, Sha256};

use super::*;
use crate::virtual_key;

static SERIAL: Mutex<()> = Mutex::new(());

fn serial() -> MutexGuard<'static, ()> {
  SERIAL.lock().unwrap_or_else(|e| e.into_inner())
}

const RP: &str = "example.com";
const CHALLENGE: &str = "AAECAwQFBgcICQoLDA0ODw";
const USER_ID: &str = "dXNlcl8x";

fn b64(s: &str) -> Vec<u8> {
  let mut bits = 0u32;
  let mut n = 0;
  let mut out = Vec::new();
  for c in s.trim_end_matches('=').bytes() {
    let v = match c {
      b'A'..=b'Z' => c - b'A',
      b'a'..=b'z' => c - b'a' + 26,
      b'0'..=b'9' => c - b'0' + 52,
      b'-' => 62,
      b'_' => 63,
      _ => panic!("not base64url"),
    };
    bits = (bits << 6) | u32::from(v);
    n += 6;
    if n >= 8 {
      n -= 8;
      out.push((bits >> n) as u8);
    }
  }
  out
}

fn create_options(resident: &str, uv: &str, exclude: &[String]) -> String {
  let exclude: Vec<Value> = exclude
    .iter()
    .map(|id| json!({ "type": "public-key", "id": id }))
    .collect();
  json!({
    "rp": { "id": RP, "name": "Example" },
    "user": { "id": USER_ID, "name": "ada@example.com", "displayName": "Ada" },
    "challenge": CHALLENGE,
    "pubKeyCredParams": [{ "type": "public-key", "alg": -7 }],
    "timeout": 30000,
    "authenticatorSelection": { "residentKey": resident, "userVerification": uv },
    "attestation": "none",
    "excludeCredentials": exclude,
  })
  .to_string()
}

fn get_options(allow: &[String], uv: &str) -> String {
  let allow: Vec<Value> = allow
    .iter()
    .map(|id| json!({ "type": "public-key", "id": id, "transports": ["usb"] }))
    .collect();
  json!({
    "challenge": CHALLENGE,
    "rpId": RP,
    "timeout": 30000,
    "userVerification": uv,
    "allowCredentials": allow,
  })
  .to_string()
}

#[derive(Default)]
struct Recorder {
  statuses: Mutex<Vec<u32>>,
  pin_asks: Mutex<Vec<i32>>,
  pins: Mutex<VecDeque<Option<String>>>,
  done: Mutex<Option<String>>,
  cv: Condvar,
  done_calls: Mutex<u32>,
}

unsafe extern "C" fn on_status(ud: *mut c_void, status: u32) {
  let r = &*(ud as *const Recorder);
  r.statuses.lock().unwrap().push(status);
}

unsafe extern "C" fn on_pin(
  ud: *mut c_void,
  attempts: i32,
  reply: *mut PinReply,
) {
  let r = &*(ud as *const Recorder);
  r.pin_asks.lock().unwrap().push(attempts);
  let answer = r.pins.lock().unwrap().pop_front().flatten();
  let c = answer.map(|p| CString::new(p).unwrap());
  laufey_webauthn_pin_reply(
    reply,
    c.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
  );
}

unsafe extern "C" fn on_done(ud: *mut c_void, doc: *const c_char) {
  let r = &*(ud as *const Recorder);
  *r.done_calls.lock().unwrap() += 1;
  *r.done.lock().unwrap() =
    Some(CStr::from_ptr(doc).to_str().unwrap().to_string());
  r.cv.notify_all();
}

struct Run {
  doc: Value,
  rec: Box<Recorder>,
  took: Duration,
}

/// Runs one ceremony; `during` runs while it is in flight (with the handle).
fn ceremony(
  kind: u32,
  options: &str,
  timeout_ms: u32,
  pins: Vec<Option<&str>>,
  during: impl FnOnce(*mut Handle),
) -> Run {
  let rec = Box::new(Recorder::default());
  *rec.pins.lock().unwrap() =
    pins.into_iter().map(|p| p.map(str::to_string)).collect();
  let callbacks = LaufeyWebauthnCallbacks {
    user_data: &*rec as *const Recorder as *mut c_void,
    status: Some(on_status),
    pin: Some(on_pin),
    done: Some(on_done),
  };
  let options = CString::new(options).unwrap();
  let rp = CString::new(RP).unwrap();
  let start = Instant::now();
  let handle = unsafe {
    laufey_webauthn_start(
      kind,
      options.as_ptr(),
      rp.as_ptr(),
      timeout_ms,
      &callbacks,
    )
  };
  assert!(!handle.is_null());
  during(handle);
  let mut done = rec.done.lock().unwrap();
  while done.is_none() {
    let (d, t) = rec.cv.wait_timeout(done, Duration::from_secs(60)).unwrap();
    done = d;
    assert!(!t.timed_out(), "the ceremony never ended");
  }
  let doc: Value = serde_json::from_str(done.as_ref().unwrap()).unwrap();
  drop(done);
  let took = start.elapsed();
  unsafe { laufey_webauthn_release(handle) };
  // Exactly once, even after a cancel raced the end.
  std::thread::sleep(Duration::from_millis(50));
  assert_eq!(*rec.done_calls.lock().unwrap(), 1);
  Run { doc, rec, took }
}

fn code(doc: &Value) -> &str {
  assert_eq!(doc["ok"], false, "{doc}");
  doc["code"].as_str().unwrap()
}

/// The registration's credential id and public key, after checking the
/// clientDataJSON and the attestation object.
fn check_registration(doc: &Value, uv: bool) -> (String, VerifyingKey) {
  assert_eq!(doc["ok"], true, "{doc}");
  let client: Value =
    serde_json::from_slice(&b64(doc["clientDataJSON"].as_str().unwrap()))
      .unwrap();
  assert_eq!(client["type"], "webauthn.create");
  assert_eq!(client["challenge"], CHALLENGE);
  assert_eq!(client["origin"], "https://example.com");
  assert_eq!(client["crossOrigin"], false);
  // WebAuthn's member order (the bytes the RP hashes).
  let raw =
    String::from_utf8(b64(doc["clientDataJSON"].as_str().unwrap())).unwrap();
  assert!(
    raw.starts_with("{\"type\":\"webauthn.create\",\"challenge\":"),
    "{raw}"
  );
  let att: Cbor = ciborium::de::from_reader(
    &b64(doc["attestationObject"].as_str().unwrap())[..],
  )
  .unwrap();
  let att = att.as_map().unwrap();
  let field = |k: &str| {
    att
      .iter()
      .find(|(key, _)| key.as_text() == Some(k))
      .map(|(_, v)| v)
      .unwrap()
  };
  assert_eq!(field("fmt").as_text(), Some("none"));
  let auth = field("authData").as_bytes().unwrap();
  assert_eq!(&auth[..32], Sha256::digest(RP.as_bytes()).as_slice());
  let flags = auth[32];
  assert!(flags & 0x01 != 0 && flags & 0x40 != 0);
  assert_eq!(flags & 0x04 != 0, uv, "UV flag");
  let id_len = u16::from_be_bytes([auth[53], auth[54]]) as usize;
  let id = &auth[55..55 + id_len];
  let id_b64 = doc["credentialId"].as_str().unwrap().to_string();
  assert_eq!(b64(&id_b64), id);
  let cose: Cbor = ciborium::de::from_reader(&auth[55 + id_len..]).unwrap();
  let cose = cose.as_map().unwrap();
  let coord = |k: i64| {
    cose
      .iter()
      .find(|(key, _)| key.as_integer().map(i128::from) == Some(k as i128))
      .and_then(|(_, v)| v.as_bytes())
      .unwrap()
      .clone()
  };
  let point = Sec1Point::from_affine_coordinates(
    coord(-2).as_slice().try_into().unwrap(),
    coord(-3).as_slice().try_into().unwrap(),
    false,
  );
  let key = VerifyingKey::from_sec1_point(&point).unwrap();
  let transports = doc["transports"].as_array().unwrap();
  assert!(transports.iter().any(|t| t == "usb"), "{doc}");
  (id_b64, key)
}

fn check_assertion(doc: &Value, key: &VerifyingKey, id: Option<&str>) {
  assert_eq!(doc["ok"], true, "{doc}");
  if let Some(id) = id {
    assert_eq!(doc["credentialId"], id);
  }
  let client_raw = b64(doc["clientDataJSON"].as_str().unwrap());
  let client: Value = serde_json::from_slice(&client_raw).unwrap();
  assert_eq!(client["type"], "webauthn.get");
  assert_eq!(client["origin"], "https://example.com");
  let auth = b64(doc["authenticatorData"].as_str().unwrap());
  assert_eq!(&auth[..32], Sha256::digest(RP.as_bytes()).as_slice());
  assert!(auth[32] & 0x01 != 0, "UP");
  let mut signed = auth.clone();
  signed.extend_from_slice(&Sha256::digest(&client_raw));
  let sig =
    Signature::from_der(&b64(doc["signature"].as_str().unwrap())).unwrap();
  key
    .verify(&signed, &sig)
    .expect("the signature verifies with the attested key");
}

#[test]
fn register_then_sign_in() {
  let _g = serial();
  virtual_key::install(None, 0);
  let run = ceremony(
    KIND_CREATE,
    &create_options("discouraged", "discouraged", &[]),
    30000,
    vec![],
    |_| {},
  );
  let (id, key) = check_registration(&run.doc, false);
  assert!(run.rec.statuses.lock().unwrap().contains(&STATUS_TOUCH_KEY));
  assert!(run.rec.pin_asks.lock().unwrap().is_empty());

  let run = ceremony(
    KIND_GET,
    &get_options(std::slice::from_ref(&id), "discouraged"),
    30000,
    vec![],
    |_| {},
  );
  check_assertion(&run.doc, &key, Some(&id));
  // A second sign-in: the counter moves, the signature still verifies.
  let run = ceremony(
    KIND_GET,
    &get_options(std::slice::from_ref(&id), "discouraged"),
    30000,
    vec![],
    |_| {},
  );
  check_assertion(&run.doc, &key, Some(&id));

  // The exclude list: this key already holds the account's credential.
  let run = ceremony(
    KIND_CREATE,
    &create_options("discouraged", "discouraged", std::slice::from_ref(&id)),
    30000,
    vec![],
    |_| {},
  );
  assert_eq!(code(&run.doc), "unknown");
  assert!(run.doc["message"]
    .as_str()
    .unwrap()
    .contains("already holds"));

  // A credential the key doesn't hold.
  let run = ceremony(
    KIND_GET,
    &get_options(&["AQIDBA".into()], "discouraged"),
    30000,
    vec![],
    |_| {},
  );
  assert_eq!(code(&run.doc), "cancelled");
  virtual_key::install(None, virtual_key::UNINSTALL);
}

#[test]
fn discoverable_credential_with_a_pin() {
  let _g = serial();
  virtual_key::install(Some("1234"), 0);
  // A wrong PIN first: asked again, then the right one.
  let run = ceremony(
    KIND_CREATE,
    &create_options("required", "required", &[]),
    30000,
    vec![Some("0000"), Some("1234")],
    |_| {},
  );
  let (id, key) = check_registration(&run.doc, true);
  let asks = run.rec.pin_asks.lock().unwrap().clone();
  assert_eq!(asks.len(), 2, "{asks:?}");
  assert!(asks[1] < 8 && asks[1] >= 0, "{asks:?}");
  assert!(run.rec.statuses.lock().unwrap().contains(&STATUS_PIN_RETRY));

  // Usernameless: no allow list; the key returns the user handle.
  let run = ceremony(
    KIND_GET,
    &get_options(&[], "required"),
    30000,
    vec![Some("1234")],
    |_| {},
  );
  check_assertion(&run.doc, &key, Some(&id));
  assert_eq!(run.doc["userHandle"], USER_ID);
  let auth = b64(run.doc["authenticatorData"].as_str().unwrap());
  assert!(auth[32] & 0x04 != 0, "UV");

  // The PIN entry cancelled.
  let run = ceremony(
    KIND_GET,
    &get_options(&[], "required"),
    30000,
    vec![None],
    |_| {},
  );
  assert_eq!(code(&run.doc), "cancelled");
  virtual_key::install(None, virtual_key::UNINSTALL);
}

#[test]
fn refused_on_the_key() {
  let _g = serial();
  virtual_key::install(None, virtual_key::FLAG_DENY);
  let run = ceremony(
    KIND_CREATE,
    &create_options("discouraged", "discouraged", &[]),
    30000,
    vec![],
    |_| {},
  );
  assert_eq!(code(&run.doc), "cancelled");
  virtual_key::install(None, virtual_key::UNINSTALL);
}

#[test]
fn no_key_plugged_in() {
  let _g = serial();
  virtual_key::install(None, virtual_key::FLAG_ABSENT);
  // Cancelled while waiting for a key: at once.
  let run = ceremony(
    KIND_CREATE,
    &create_options("discouraged", "discouraged", &[]),
    30000,
    vec![],
    |h| {
      std::thread::sleep(Duration::from_millis(400));
      unsafe { laufey_webauthn_cancel(h) };
      unsafe { laufey_webauthn_cancel(h) }; // repeatable
    },
  );
  assert_eq!(code(&run.doc), "cancelled");
  assert!(run.took < Duration::from_secs(3), "{:?}", run.took);
  assert_eq!(
    run.rec.statuses.lock().unwrap().first(),
    Some(&STATUS_INSERT_KEY)
  );

  // The timeout.
  let run = ceremony(
    KIND_CREATE,
    &create_options("discouraged", "discouraged", &[]),
    1500,
    vec![],
    |_| {},
  );
  assert_eq!(code(&run.doc), "timeout");
  assert!(
    run.took >= Duration::from_millis(1400)
      && run.took < Duration::from_secs(5),
    "{:?}",
    run.took
  );

  // Plugged in while waiting: the ceremony goes on with it.
  let run = ceremony(
    KIND_CREATE,
    &create_options("discouraged", "discouraged", &[]),
    30000,
    vec![],
    |_| {
      std::thread::sleep(Duration::from_millis(600));
      virtual_key::set_flags(0);
    },
  );
  check_registration(&run.doc, false);
  virtual_key::install(None, virtual_key::UNINSTALL);
}

#[test]
fn options_libwebauthn_refuses() {
  let _g = serial();
  virtual_key::install(None, 0);
  let run = ceremony(KIND_CREATE, "{\"rp\":{}}", 30000, vec![], |_| {});
  assert_eq!(code(&run.doc), "unknown");
  assert!(run.doc["message"]
    .as_str()
    .unwrap()
    .starts_with("invalid options"));
  let run =
    ceremony(7, &get_options(&[], "discouraged"), 30000, vec![], |_| {});
  assert_eq!(code(&run.doc), "unknown");
  virtual_key::install(None, virtual_key::UNINSTALL);
}

#[test]
fn null_arguments_start_nothing() {
  let cb = LaufeyWebauthnCallbacks {
    user_data: std::ptr::null_mut(),
    status: None,
    pin: None,
    done: None,
  };
  let rp = CString::new(RP).unwrap();
  let h = unsafe {
    laufey_webauthn_start(KIND_GET, std::ptr::null(), rp.as_ptr(), 1000, &cb)
  };
  assert!(h.is_null());
  unsafe { laufey_webauthn_cancel(std::ptr::null_mut()) };
  unsafe { laufey_webauthn_release(std::ptr::null_mut()) };
  unsafe { laufey_webauthn_pin_reply(std::ptr::null_mut(), std::ptr::null()) };
}

#[test]
fn capabilities() {
  let _g = serial();
  assert_eq!(laufey_webauthn_abi(), ABI_VERSION);
  virtual_key::install(None, 0);
  let mut reason = [0 as c_char; 128];
  let caps =
    unsafe { laufey_webauthn_capabilities(reason.as_mut_ptr(), reason.len()) };
  assert_eq!(caps, CAP_SECURITY_KEYS);
  assert_eq!(reason[0], 0);
  virtual_key::install(None, virtual_key::UNINSTALL);
}
