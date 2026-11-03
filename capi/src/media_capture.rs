// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Microphone and camera requests (API 53). See `docs/media-capture.md`.
//!
//! A page's `getUserMedia` reaches the backend, which refuses a request from
//! a sub-frame of another origin (or for screen capture), allows one the
//! launch file's `mediaCapture` grants, and asks the handler registered here
//! about the rest. With no handler, those are refused.

use std::ffi::{c_char, c_void, CStr};
use std::sync::{Arc, Mutex, OnceLock};

use crate::{api, LaufeyBackendApi};

/// What a page asks to capture (`LAUFEY_MEDIA_CAPTURE_*` in `laufey.h`): a
/// set of [`MediaCaptureKinds::MICROPHONE`] and [`MediaCaptureKinds::CAMERA`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct MediaCaptureKinds(pub u32);

impl MediaCaptureKinds {
  /// `getUserMedia({ audio })`.
  pub const MICROPHONE: MediaCaptureKinds = MediaCaptureKinds(1 << 0);
  /// `getUserMedia({ video })`.
  pub const CAMERA: MediaCaptureKinds = MediaCaptureKinds(1 << 1);

  /// Whether the microphone is asked for.
  pub fn microphone(self) -> bool {
    self.0 & Self::MICROPHONE.0 != 0
  }

  /// Whether the camera is asked for.
  pub fn camera(self) -> bool {
    self.0 & Self::CAMERA.0 != 0
  }

  /// Whether every kind in `other` is in `self`.
  pub fn contains(self, other: MediaCaptureKinds) -> bool {
    self.0 & other.0 == other.0
  }
}

impl std::ops::BitOr for MediaCaptureKinds {
  type Output = MediaCaptureKinds;
  fn bitor(self, rhs: MediaCaptureKinds) -> MediaCaptureKinds {
    MediaCaptureKinds(self.0 | rhs.0)
  }
}

/// A page's request for the microphone and / or camera, waiting for
/// [`answer_media_capture`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MediaCaptureRequest {
  /// The window whose page asks.
  pub window_id: u32,
  /// Names the request for [`answer_media_capture`].
  pub id: u64,
  /// The asking document's serialized origin (`scheme://host[:port]`), which
  /// is also the window's top-level document's.
  pub origin: String,
  /// What it asks for.
  pub kinds: MediaCaptureKinds,
}

type Handler = Arc<dyn Fn(MediaCaptureRequest) + Send + Sync>;

fn handler_slot() -> &'static Mutex<Option<Handler>> {
  static SLOT: OnceLock<Mutex<Option<Handler>>> = OnceLock::new();
  SLOT.get_or_init(|| Mutex::new(None))
}

/// Decodes the backend's arguments; `None` for a request that can't be one
/// (no origin, or one that isn't UTF-8).
fn request_from_raw(
  window_id: u32,
  request_id: u64,
  origin: *const c_char,
  kinds: u32,
) -> Option<MediaCaptureRequest> {
  if origin.is_null() {
    return None;
  }
  let origin = unsafe { CStr::from_ptr(origin) }.to_str().ok()?.to_string();
  Some(MediaCaptureRequest {
    window_id,
    id: request_id,
    origin,
    kinds: MediaCaptureKinds(kinds),
  })
}

fn deliver(
  api: &LaufeyBackendApi,
  window_id: u32,
  request_id: u64,
  origin: *const c_char,
  kinds: u32,
) {
  let handler = handler_slot().lock().unwrap().clone();
  match (
    request_from_raw(window_id, request_id, origin, kinds),
    handler,
  ) {
    (Some(request), Some(handler)) => handler(request),
    // Nobody can answer it: refuse it now rather than leave the page waiting.
    _ => {
      answer_media_capture_with(api, request_id, false);
    }
  }
}

unsafe extern "C" fn trampoline(
  window_id: u32,
  request_id: u64,
  origin: *const c_char,
  kinds: u32,
  _user_data: *mut c_void,
) {
  deliver(api(), window_id, request_id, origin, kinds);
}

pub(crate) fn on_media_capture_request_with<F>(
  api: &LaufeyBackendApi,
  handler: F,
) -> bool
where
  F: Fn(MediaCaptureRequest) + Send + Sync + 'static,
{
  let Some(f) = api.set_media_capture_handler else {
    return false;
  };
  *handler_slot().lock().unwrap() = Some(Arc::new(handler));
  unsafe { f(api.backend_data, Some(trampoline), std::ptr::null_mut()) };
  true
}

/// Register the (single, process-wide) handler asked about the microphone and
/// camera requests the launch file doesn't already grant. It fires on the UI
/// thread; answer each request with [`answer_media_capture`] (from any
/// thread), or the page's `getUserMedia` keeps waiting. Replaces any previous
/// handler. Returns `false` when the backend can't capture media (no web
/// engine, or older than API 53).
pub fn on_media_capture_request<F>(handler: F) -> bool
where
  F: Fn(MediaCaptureRequest) + Send + Sync + 'static,
{
  on_media_capture_request_with(api(), handler)
}

pub(crate) fn clear_media_capture_handler_with(api: &LaufeyBackendApi) {
  *handler_slot().lock().unwrap() = None;
  if let Some(f) = api.set_media_capture_handler {
    unsafe { f(api.backend_data, None, std::ptr::null_mut()) };
  }
}

/// Remove the handler: every request still waiting is refused, and requests
/// the launch file doesn't grant are refused from now on.
pub fn clear_media_capture_handler() {
  clear_media_capture_handler_with(api());
}

fn answer_media_capture_with(
  api: &LaufeyBackendApi,
  request_id: u64,
  allow: bool,
) -> bool {
  match api.answer_media_capture {
    Some(f) => unsafe { f(api.backend_data, request_id, allow) },
    None => false,
  }
}

/// Answer request `id`: `true` allows every kind it asked for. Returns
/// `false` when it is not waiting (already answered, refused because its
/// window closed or the handler was cleared, or never asked). Any thread.
pub fn answer_media_capture(id: u64, allow: bool) -> bool {
  answer_media_capture_with(api(), id, allow)
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::ffi::CString;
  use std::sync::atomic::{AtomicBool, Ordering};

  static ANSWERS: Mutex<Vec<(u64, bool)>> = Mutex::new(Vec::new());
  static HANDLER_SET: AtomicBool = AtomicBool::new(false);
  // The slot is process-wide; the tests that use it take turns.
  static SERIAL: Mutex<()> = Mutex::new(());

  unsafe extern "C" fn fake_set(
    _: *mut c_void,
    handler: Option<
      unsafe extern "C" fn(u32, u64, *const c_char, u32, *mut c_void),
    >,
    _: *mut c_void,
  ) {
    HANDLER_SET.store(handler.is_some(), Ordering::SeqCst);
  }

  unsafe extern "C" fn fake_answer(
    _: *mut c_void,
    id: u64,
    allow: bool,
  ) -> bool {
    ANSWERS.lock().unwrap().push((id, allow));
    true
  }

  fn fake_api() -> LaufeyBackendApi {
    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    api.set_media_capture_handler = Some(fake_set);
    api.answer_media_capture = Some(fake_answer);
    api
  }

  #[test]
  fn the_kinds_decode() {
    let both = MediaCaptureKinds::MICROPHONE | MediaCaptureKinds::CAMERA;
    assert!(both.microphone() && both.camera());
    assert!(both.contains(MediaCaptureKinds::CAMERA));
    assert!(!MediaCaptureKinds::MICROPHONE.contains(both));
    assert!(!MediaCaptureKinds::CAMERA.microphone());
    assert_eq!(MediaCaptureKinds::MICROPHONE.0, 1);
    assert_eq!(MediaCaptureKinds::CAMERA.0, 2);
  }

  #[test]
  fn a_request_decodes() {
    let origin = CString::new("app://localhost").unwrap();
    assert_eq!(
      request_from_raw(3, 9, origin.as_ptr(), 1),
      Some(MediaCaptureRequest {
        window_id: 3,
        id: 9,
        origin: "app://localhost".into(),
        kinds: MediaCaptureKinds::MICROPHONE,
      })
    );
    assert_eq!(request_from_raw(3, 9, std::ptr::null(), 1), None);
    let bad = [0xffu8, 0];
    assert_eq!(
      request_from_raw(3, 9, bad.as_ptr() as *const c_char, 1),
      None
    );
  }

  #[test]
  fn not_supported_without_the_entry_points() {
    let _serial = SERIAL.lock().unwrap();
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert!(!on_media_capture_request_with(&none, |_| {}));
    assert!(!answer_media_capture_with(&none, 1, true));
    clear_media_capture_handler_with(&none);
  }

  #[test]
  fn the_handler_is_asked_and_cleared() {
    let _serial = SERIAL.lock().unwrap();
    let api = fake_api();
    let seen: Arc<Mutex<Vec<MediaCaptureRequest>>> =
      Arc::new(Mutex::new(Vec::new()));
    let s = seen.clone();
    assert!(on_media_capture_request_with(&api, move |r| {
      s.lock().unwrap().push(r)
    }));
    assert!(HANDLER_SET.load(Ordering::SeqCst));
    let origin = CString::new("myapp://app").unwrap();
    deliver(&api, 1, 42, origin.as_ptr(), 3);
    assert_eq!(seen.lock().unwrap().len(), 1);
    assert_eq!(seen.lock().unwrap()[0].id, 42);
    assert!(seen.lock().unwrap()[0].kinds.camera());

    // A request that can't be decoded is refused, not dropped.
    ANSWERS.lock().unwrap().clear();
    deliver(&api, 1, 43, std::ptr::null(), 1);
    assert_eq!(*ANSWERS.lock().unwrap(), vec![(43, false)]);
    assert_eq!(seen.lock().unwrap().len(), 1);

    // With the handler cleared, a request that still arrives is refused.
    clear_media_capture_handler_with(&api);
    assert!(!HANDLER_SET.load(Ordering::SeqCst));
    ANSWERS.lock().unwrap().clear();
    deliver(&api, 1, 44, origin.as_ptr(), 1);
    assert_eq!(*ANSWERS.lock().unwrap(), vec![(44, false)]);
    assert!(answer_media_capture_with(&api, 45, true));
  }
}
