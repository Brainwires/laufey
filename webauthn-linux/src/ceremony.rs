// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The ceremonies and the C ABI (`laufey_webauthn_abi.h` in backend-common).

use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use libwebauthn::ops::webauthn::{
  GetAssertionRequest, MakeCredentialRequest, OriginValidation, RequestOrigin,
  RequestSettings, WebAuthnIDLResponse,
};
use libwebauthn::transport::hid::channel::HidChannelHandle;
use libwebauthn::transport::hid::{list_devices, HidDevice};
use libwebauthn::transport::{Channel, ChannelSettings, Device};
use libwebauthn::webauthn::{
  CtapError, PlatformError, WebAuthn, WebAuthnError,
};
use libwebauthn::UvUpdate;
use serde_json::{json, Value};
use tokio::sync::{broadcast, oneshot, Notify};

/// The ABI this library speaks (`LAUFEY_WEBAUTHN_ABI_VERSION`).
pub const ABI_VERSION: u32 = 1;

/// `laufey_webauthn_start`'s kinds (the values of `LAUFEY_PASSKEY_CREATE` /
/// `_GET`).
pub const KIND_CREATE: u32 = 0;
pub const KIND_GET: u32 = 1;

/// Capability bit: security keys over USB (HID) can be used here.
pub const CAP_SECURITY_KEYS: u32 = 1 << 1;

/// What the user is asked to do now (`status` callback).
pub const STATUS_INSERT_KEY: u32 = 1;
pub const STATUS_SELECT_KEY: u32 = 2;
pub const STATUS_TOUCH_KEY: u32 = 3;
pub const STATUS_PIN_RETRY: u32 = 4;

/// How often the list of keys is read again while none is plugged in.
const DEVICE_POLL: Duration = Duration::from_millis(250);
/// Retries of a ceremony after a wrong PIN or a key that wasn't touched in
/// its own time (the overall timeout still applies).
const MAX_RETRIES: u32 = 8;

/// The callbacks of one ceremony. Called on the ceremony's own thread.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LaufeyWebauthnCallbacks {
  pub user_data: *mut c_void,
  /// A `STATUS_*` (what to tell the user). May be null.
  pub status: Option<unsafe extern "C" fn(*mut c_void, u32)>,
  /// The key needs its PIN: `attempts_left` (-1 when unknown) and the reply,
  /// which must be answered exactly once with `laufey_webauthn_pin_reply`
  /// (null cancels). Null: every PIN request is cancelled.
  pub pin: Option<unsafe extern "C" fn(*mut c_void, i32, *mut PinReply)>,
  /// The result document, exactly once, at the end (copy it).
  pub done: Option<unsafe extern "C" fn(*mut c_void, *const c_char)>,
}

// The embedder promises the pointers may be used from the ceremony's thread
// (and the callbacks are only ever called from it).
unsafe impl Send for LaufeyWebauthnCallbacks {}
unsafe impl Sync for LaufeyWebauthnCallbacks {}

/// A PIN request's answer slot, handed to the `pin` callback.
pub struct PinReply {
  tx: Option<oneshot::Sender<Option<String>>>,
}

/// A running ceremony, for `laufey_webauthn_cancel` / `_release`.
pub struct Handle {
  shared: Arc<Shared>,
}

#[derive(Default)]
struct Shared {
  cancelled: AtomicBool,
  notify: Notify,
  channel: Mutex<Option<HidChannelHandle>>,
}

impl Shared {
  fn cancelled(&self) -> bool {
    self.cancelled.load(Ordering::SeqCst)
  }
}

/// A result document: `{"ok":false,"code":…,"message":…}`.
fn error_doc(code: &str, message: &str) -> String {
  json!({ "ok": false, "code": code, "message": message }).to_string()
}

/// How a ceremony ended: a result document.
type Outcome = String;

// --- Errors ---------------------------------------------------------------------

/// A libwebauthn error as laufey's envelope code and a message that names no
/// value (the codes of `@clerk/electron-passkeys`; the DOMException a browser
/// would raise decides where they differ: NotAllowedError is `cancelled`).
pub fn map_error<E: std::fmt::Display>(
  error: &WebAuthnError<E>,
) -> (&'static str, String) {
  match error {
    WebAuthnError::Ctap(c) => match c {
      CtapError::OperationDenied
      | CtapError::KeepAliveCancel
      | CtapError::NotAllowed => (
        "cancelled",
        "the request was refused on the security key".into(),
      ),
      CtapError::UserActionTimeout
      | CtapError::ActionTimeout
      | CtapError::Timeout => {
        ("timeout", "the security key wasn't touched in time".into())
      }
      CtapError::CredentialExcluded => (
        "unknown",
        "the security key already holds a credential for this account".into(),
      ),
      CtapError::NoCredentials | CtapError::InvalidCredential => (
        "cancelled",
        "the security key holds no credential for this site".into(),
      ),
      CtapError::PINBlocked
      | CtapError::PINAuthBlocked
      | CtapError::UvBlocked => (
        "unknown",
        "the security key's PIN is blocked (too many wrong attempts)".into(),
      ),
      CtapError::PINInvalid
      | CtapError::PINAuthInvalid
      | CtapError::UVInvalid => ("unknown", "the PIN was wrong".into()),
      CtapError::PINNotSet | CtapError::PINRequired => (
        "not_supported",
        "user verification needs a PIN, and the security key has none set"
          .into(),
      ),
      CtapError::UnsupportedAlgorithm
      | CtapError::UnsupportedOption
      | CtapError::UnsupportedExtension => (
        "not_supported",
        "the security key supports none of the requested algorithms or options"
          .into(),
      ),
      CtapError::KeyStoreFull => (
        "unknown",
        "the security key has no room for another passkey".into(),
      ),
      other => ("unknown", format!("the security key answered {other:?}")),
    },
    WebAuthnError::Platform(p) => match p {
      PlatformError::Cancelled => {
        ("cancelled", "the PIN entry was cancelled".into())
      }
      PlatformError::PinTooShort => ("unknown", "the PIN is too short".into()),
      PlatformError::PinTooLong => ("unknown", "the PIN is too long".into()),
      PlatformError::NoUvAvailable | PlatformError::PinNotSupported => (
        "not_supported",
        "the security key can't verify the user".into(),
      ),
      PlatformError::NotSupported => (
        "not_supported",
        "the security key doesn't support this request".into(),
      ),
      PlatformError::RequestTooLarge => (
        "not_supported",
        "the request exceeds the security key's limits".into(),
      ),
      other => ("unknown", format!("the request failed: {other}")),
    },
    WebAuthnError::Transport(t) => (
      "unknown",
      format!("the security key's connection failed: {t}"),
    ),
    _ => ("unknown", "the request failed".into()),
  }
}

fn retryable<E>(error: &WebAuthnError<E>) -> bool {
  matches!(
    error,
    WebAuthnError::Ctap(
      CtapError::PINInvalid
        | CtapError::UVInvalid
        | CtapError::UserActionTimeout
    )
  )
}

// --- Devices ----------------------------------------------------------------------

async fn find_devices() -> Result<Vec<HidDevice>, String> {
  #[cfg(any(test, feature = "virt-test"))]
  if let Some(devices) = crate::virtual_key::devices() {
    return Ok(devices);
  }
  list_devices().await.map_err(|e| e.to_string())
}

/// The key to use: the only one plugged in, or (several) the one the user
/// touches first; waits for one to be plugged in.
async fn wait_for_key(
  cb: &Ui,
  shared: &Arc<Shared>,
  deadline: Instant,
) -> Result<HidDevice, Outcome> {
  let mut asked = false;
  loop {
    let devices = find_devices().await.map_err(|e| {
      error_doc(
        "not_supported",
        &format!("security keys can't be listed: {e}"),
      )
    })?;
    match devices.len() {
      0 => {
        if !asked {
          cb.status(STATUS_INSERT_KEY);
          asked = true;
        }
      }
      1 => return Ok(devices.into_iter().next().unwrap()),
      _ => {
        cb.status(STATUS_SELECT_KEY);
        if let Some(device) = select_key(devices, shared, deadline).await {
          return Ok(device);
        }
      }
    }
    if Instant::now() >= deadline {
      return Err(error_doc("timeout", "no security key was used in time"));
    }
    tokio::time::sleep(DEVICE_POLL).await;
  }
}

/// Several keys: each blinks, and the first one touched is the one (the
/// others are cancelled). None when no key was touched (all gave up).
async fn select_key(
  devices: Vec<HidDevice>,
  shared: &Arc<Shared>,
  deadline: Instant,
) -> Option<HidDevice> {
  let wait = deadline.saturating_duration_since(Instant::now());
  let (touched_tx, mut touched_rx) =
    tokio::sync::mpsc::channel::<Option<usize>>(devices.len());
  let handles: Arc<Mutex<Vec<HidChannelHandle>>> = Arc::default();
  let count = devices.len();
  for (i, mut device) in devices.clone().into_iter().enumerate() {
    let tx = touched_tx.clone();
    let handles = handles.clone();
    tokio::spawn(async move {
      let touched = match device.channel(ChannelSettings::default()).await {
        Ok(mut channel) => {
          handles.lock().unwrap().push(channel.get_handle());
          matches!(
            channel.blink_and_wait_for_user_presence(wait).await,
            Ok(true)
          )
        }
        Err(_) => false,
      };
      let _ = tx.send(touched.then_some(i)).await;
    });
  }
  drop(touched_tx);
  let mut answered = 0;
  let mut chosen = None;
  while let Some(answer) = touched_rx.recv().await {
    answered += 1;
    if let Some(i) = answer {
      chosen = Some(i);
      break;
    }
    if answered == count || shared.cancelled() {
      break;
    }
  }
  let others: Vec<HidChannelHandle> =
    handles.lock().unwrap().drain(..).collect();
  for h in others {
    h.cancel_ongoing_operation().await;
  }
  chosen.map(|i| devices[i].clone())
}

// --- The ceremony -------------------------------------------------------------------

/// The callbacks, used from the ceremony's thread.
struct Ui {
  callbacks: LaufeyWebauthnCallbacks,
  /// A PIN request was answered with a cancel (libwebauthn then fails the
  /// request with whatever error the key's state gives).
  pin_cancelled: AtomicBool,
}

impl Ui {
  fn status(&self, status: u32) {
    if let Some(f) = self.callbacks.status {
      unsafe { f(self.callbacks.user_data, status) };
    }
  }

  /// Asks for the PIN; None cancels.
  async fn pin(&self, attempts_left: Option<u32>) -> Option<String> {
    let f = self.callbacks.pin?;
    let (tx, rx) = oneshot::channel();
    let reply = Box::into_raw(Box::new(PinReply { tx: Some(tx) }));
    let attempts = attempts_left
      .map(|a| a.min(i32::MAX as u32) as i32)
      .unwrap_or(-1);
    unsafe { f(self.callbacks.user_data, attempts, reply) };
    let pin = rx.await.ok().flatten().filter(|p| !p.is_empty());
    if pin.is_none() {
      self.pin_cancelled.store(true, Ordering::SeqCst);
    }
    pin
  }

  fn done(&self, doc: &str) {
    if let Some(f) = self.callbacks.done {
      let c = CString::new(doc.replace('\0', "")).unwrap_or_default();
      unsafe { f(self.callbacks.user_data, c.as_ptr()) };
    }
  }
}

/// Forwards libwebauthn's user-verification updates until the channel goes.
async fn forward_updates(mut rx: broadcast::Receiver<UvUpdate>, ui: Arc<Ui>) {
  loop {
    match rx.recv().await {
      Ok(UvUpdate::PresenceRequired) => ui.status(STATUS_TOUCH_KEY),
      Ok(UvUpdate::UvRetry { .. }) => ui.status(STATUS_PIN_RETRY),
      Ok(UvUpdate::PinRequired(update)) => {
        let attempts = update.attempts_left;
        match ui.pin(attempts).await {
          Some(pin) => {
            let _ = update.send_pin(&pin);
          }
          None => update.cancel(),
        }
      }
      // Setting a PIN is the key's owner's business (its own tool), never a
      // side effect of signing in.
      Ok(UvUpdate::PinNotSet(update)) => update.cancel(),
      Err(broadcast::error::RecvError::Lagged(_)) => continue,
      Err(broadcast::error::RecvError::Closed) => return,
    }
  }
}

fn origin_of(rp_id: &str) -> Result<RequestOrigin, Outcome> {
  RequestOrigin::try_from(format!("https://{rp_id}").as_str())
    .map_err(|_| error_doc("invalid_rp", "the RP ID is not a valid domain"))
}

// laufey's backend has checked the RP ID (its own rule and the app's
// passkeyRpIds pin) and the origin is built from it, so libwebauthn is told
// to trust the binding (no public-suffix list involved).
fn settings() -> RequestSettings<'static> {
  RequestSettings {
    origin: OriginValidation::Trust,
  }
}

fn registration_doc(model: &Value) -> Outcome {
  let r = &model["response"];
  json!({
    "ok": true,
    "credentialId": model["rawId"],
    "clientDataJSON": r["clientDataJSON"],
    "attestationObject": r["attestationObject"],
    "transports": r["transports"],
    "attachment": model.get("authenticatorAttachment").cloned().unwrap_or(Value::Null),
  })
  .to_string()
}

fn assertion_doc(model: &Value) -> Outcome {
  let r = &model["response"];
  json!({
    "ok": true,
    "credentialId": model["rawId"],
    "clientDataJSON": r["clientDataJSON"],
    "authenticatorData": r["authenticatorData"],
    "signature": r["signature"],
    "userHandle": r.get("userHandle").cloned().unwrap_or(Value::Null),
    "attachment": model.get("authenticatorAttachment").cloned().unwrap_or(Value::Null),
  })
  .to_string()
}

enum Request {
  Create(MakeCredentialRequest),
  Get(GetAssertionRequest),
}

async fn prepare(
  kind: u32,
  options: &str,
  rp_id: &str,
) -> Result<Request, Outcome> {
  let origin = origin_of(rp_id)?;
  let invalid =
    |what: &str| error_doc("unknown", &format!("invalid options: {what}"));
  match kind {
    KIND_CREATE => {
      MakeCredentialRequest::prepare(&origin, options, &settings())
        .await
        .map(Request::Create)
        .map_err(|e| invalid(&e.to_string()))
    }
    KIND_GET => GetAssertionRequest::prepare(&origin, options, &settings())
      .await
      .map(Request::Get)
      .map_err(|e| invalid(&e.to_string())),
    _ => Err(invalid("unknown request kind")),
  }
}

async fn ceremony(
  kind: u32,
  options: String,
  rp_id: String,
  deadline: Instant,
  shared: Arc<Shared>,
  ui: Arc<Ui>,
) -> Outcome {
  let request = match prepare(kind, &options, &rp_id).await {
    Ok(r) => r,
    Err(doc) => return doc,
  };
  let mut device = match wait_for_key(&ui, &shared, deadline).await {
    Ok(d) => d,
    Err(doc) => return doc,
  };
  let mut channel = match device.channel(ChannelSettings::default()).await {
    Ok(c) => c,
    Err(e) => {
      let (code, message) = map_error(&e);
      return error_doc(code, &message);
    }
  };
  *shared.channel.lock().unwrap() = Some(channel.get_handle());
  let mut updates = tokio::spawn(forward_updates(
    channel.get_ux_update_receiver(),
    ui.clone(),
  ));
  let mut attempts = 0;
  let doc = loop {
    let result = match &request {
      Request::Create(req) => {
        channel.webauthn_make_credential(req).await.map(|resp| {
          match resp.to_json_value(req) {
            Ok(model) => registration_doc(&model),
            Err(_) => {
              error_doc("unknown", "the security key's answer can't be encoded")
            }
          }
        })
      }
      Request::Get(req) => {
        channel.webauthn_get_assertion(req).await.map(|resp| {
          // With several discoverable credentials for the RP, the first the
          // key returns (a browser would show an account picker).
          match resp.assertions.first().map(|a| a.to_json_value(req)) {
            Some(Ok(model)) => assertion_doc(&model),
            _ => error_doc("unknown", "the security key returned no assertion"),
          }
        })
      }
    };
    match result {
      Ok(doc) => break doc,
      Err(e)
        if retryable(&e)
          && attempts < MAX_RETRIES
          && Instant::now() < deadline =>
      {
        attempts += 1;
        ui.status(STATUS_PIN_RETRY);
      }
      Err(_) if ui.pin_cancelled.load(Ordering::SeqCst) => {
        break error_doc("cancelled", "the PIN entry was cancelled");
      }
      Err(e) => {
        let (code, message) = map_error(&e);
        break error_doc(code, &message);
      }
    }
  };
  *shared.channel.lock().unwrap() = None;
  // Closing the channel ends the updates once the ones sent are forwarded.
  drop(channel);
  if tokio::time::timeout(Duration::from_millis(500), &mut updates)
    .await
    .is_err()
  {
    updates.abort();
  }
  doc
}

fn run(
  kind: u32,
  options: String,
  rp_id: String,
  timeout: Duration,
  shared: Arc<Shared>,
  ui: Arc<Ui>,
) {
  let deadline = Instant::now() + timeout;
  // `done` runs whatever happens: a panic in the ceremony (libwebauthn's or
  // ours) is an `unknown` result, never a request that never ends.
  let done_ui = ui.clone();
  let doc = std::panic::catch_unwind(std::panic::AssertUnwindSafe(move || {
    ceremony_thread(kind, options, rp_id, deadline, shared, ui)
  }))
  .unwrap_or_else(|_| error_doc("unknown", "the security key request failed"));
  done_ui.done(&doc);
}

fn ceremony_thread(
  kind: u32,
  options: String,
  rp_id: String,
  deadline: Instant,
  shared: Arc<Shared>,
  ui: Arc<Ui>,
) -> Outcome {
  match tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
  {
    Err(_) => error_doc("unknown", "the passkey worker couldn't start"),
    Ok(rt) => {
      let doc = rt.block_on(async {
        let sleep = tokio::time::sleep_until(tokio::time::Instant::from_std(deadline));
        tokio::select! {
          doc = ceremony(kind, options, rp_id, deadline, shared.clone(), ui.clone()) => doc,
          _ = shared.notify.notified() => {
            cancel_channel(&shared).await;
            error_doc("cancelled", "the request was cancelled")
          }
          _ = sleep => {
            cancel_channel(&shared).await;
            error_doc("timeout", "the request timed out")
          }
        }
      });
      // A key read in flight sees the cancel at its next poll.
      rt.shutdown_timeout(Duration::from_secs(2));
      doc
    }
  }
}

async fn cancel_channel(shared: &Shared) {
  let handle = shared.channel.lock().unwrap().take();
  if let Some(h) = handle {
    h.cancel_ongoing_operation().await;
  }
}

// --- C ABI ----------------------------------------------------------------------

/// The ABI version (`LAUFEY_WEBAUTHN_ABI_VERSION`).
#[no_mangle]
pub extern "C" fn laufey_webauthn_abi() -> u32 {
  ABI_VERSION
}

fn write_reason(out: *mut c_char, len: usize, text: &str) {
  if out.is_null() || len == 0 {
    return;
  }
  let bytes = text.as_bytes();
  let n = bytes.len().min(len - 1);
  unsafe {
    std::ptr::copy_nonoverlapping(bytes.as_ptr(), out.cast::<u8>(), n);
    *out.add(n) = 0;
  }
}

/// `CAP_SECURITY_KEYS` when keys can be listed here (hidapi over hidraw,
/// with or without a key plugged in), else 0 with why in `reason` (`len`
/// bytes, NUL-terminated; may be null).
///
/// # Safety
/// `reason` is null or points to `len` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn laufey_webauthn_capabilities(
  reason: *mut c_char,
  len: usize,
) -> u32 {
  write_reason(reason, len, "");
  let rt = match tokio::runtime::Builder::new_current_thread()
    .enable_all()
    .build()
  {
    Ok(rt) => rt,
    Err(_) => {
      write_reason(reason, len, "the passkey worker couldn't start");
      return 0;
    }
  };
  match rt.block_on(find_devices()) {
    Ok(_) => CAP_SECURITY_KEYS,
    Err(e) => {
      write_reason(reason, len, &format!("security keys can't be listed: {e}"));
      0
    }
  }
}

/// Starts a ceremony: `kind` `KIND_CREATE` / `KIND_GET`, `options` the
/// options document (WebAuthn JSON, already validated), `rp_id` its RP ID,
/// `timeout_ms` the whole ceremony's time. Returns the handle (release it
/// with `laufey_webauthn_release` once `done` ran), or null for a null
/// argument or text that isn't UTF-8 (`done` is then never called).
///
/// # Safety
/// The strings are NUL-terminated; `callbacks` points to a struct whose
/// pointers stay valid until `done` has run.
#[no_mangle]
pub unsafe extern "C" fn laufey_webauthn_start(
  kind: u32,
  options: *const c_char,
  rp_id: *const c_char,
  timeout_ms: u32,
  callbacks: *const LaufeyWebauthnCallbacks,
) -> *mut Handle {
  if options.is_null() || rp_id.is_null() || callbacks.is_null() {
    return std::ptr::null_mut();
  }
  let (Ok(options), Ok(rp_id)) = (
    CStr::from_ptr(options).to_str(),
    CStr::from_ptr(rp_id).to_str(),
  ) else {
    return std::ptr::null_mut();
  };
  let (options, rp_id) = (options.to_string(), rp_id.to_string());
  let ui = Arc::new(Ui {
    callbacks: *callbacks,
    pin_cancelled: AtomicBool::new(false),
  });
  let shared: Arc<Shared> = Arc::default();
  let timeout = Duration::from_millis(u64::from(timeout_ms.max(1000)));
  let worker = shared.clone();
  let spawned = std::thread::Builder::new()
    .name("laufey-webauthn".into())
    .spawn(move || run(kind, options, rp_id, timeout, worker, ui));
  if spawned.is_err() {
    return std::ptr::null_mut();
  }
  Box::into_raw(Box::new(Handle { shared }))
}

/// Cancels the ceremony (`done` then answers `cancelled`, unless it already
/// ran). Any thread; repeatable.
///
/// # Safety
/// `handle` comes from `laufey_webauthn_start` and wasn't released.
#[no_mangle]
pub unsafe extern "C" fn laufey_webauthn_cancel(handle: *mut Handle) {
  if let Some(h) = handle.as_ref() {
    if !h.shared.cancelled.swap(true, Ordering::SeqCst) {
      h.shared.notify.notify_one();
    }
  }
}

/// Frees the handle (the ceremony may still be finishing; `done` is still
/// called once).
///
/// # Safety
/// `handle` comes from `laufey_webauthn_start`; released once.
#[no_mangle]
pub unsafe extern "C" fn laufey_webauthn_release(handle: *mut Handle) {
  if !handle.is_null() {
    drop(Box::from_raw(handle));
  }
}

/// Answers a PIN request: `pin` (UTF-8), or null / empty to cancel. Frees
/// `reply`.
///
/// # Safety
/// `reply` comes from the `pin` callback and is answered once.
#[no_mangle]
pub unsafe extern "C" fn laufey_webauthn_pin_reply(
  reply: *mut PinReply,
  pin: *const c_char,
) {
  if reply.is_null() {
    return;
  }
  let mut reply = Box::from_raw(reply);
  let pin = if pin.is_null() {
    None
  } else {
    CStr::from_ptr(pin).to_str().ok().map(str::to_string)
  };
  if let Some(tx) = reply.tx.take() {
    let _ = tx.send(pin);
  }
}

#[cfg(test)]
mod tests;
