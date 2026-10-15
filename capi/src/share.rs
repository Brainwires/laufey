// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The share sheet (API 49): macOS NSSharingServicePicker, Windows
//! DataTransferManager; none on Linux. See `docs/share.md`.

use std::ffi::{c_char, c_int, c_void, CStr, CString};

use crate::{api, LaufeyBackendApi};

/// What a share request may carry; at least one of `text`, `url` or
/// `files`.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct ShareRequest {
  pub title: Option<String>,
  pub text: Option<String>,
  pub url: Option<String>,
  /// Absolute paths.
  pub files: Vec<String>,
  /// Where to anchor the macOS picker, in the window's DIPs.
  pub point: Option<(i32, i32)>,
}

/// How a share ended.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ShareOutcome {
  /// A target was chosen (its name where the OS says).
  Completed(Option<String>),
  /// The share UI opened; this OS reports nothing more.
  Shown,
  Cancelled,
  NotSupported(Option<String>),
  Failed(Option<String>),
}

fn json_string(s: &str) -> String {
  let mut out = String::with_capacity(s.len() + 2);
  out.push('"');
  for c in s.chars() {
    match c {
      '"' => out.push_str("\\\""),
      '\\' => out.push_str("\\\\"),
      c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
      c => out.push(c),
    }
  }
  out.push('"');
  out
}

impl ShareRequest {
  fn to_json(&self) -> String {
    let mut parts = Vec::new();
    if let Some(t) = &self.title {
      parts.push(format!("\"title\":{}", json_string(t)));
    }
    if let Some(t) = &self.text {
      parts.push(format!("\"text\":{}", json_string(t)));
    }
    if let Some(u) = &self.url {
      parts.push(format!("\"url\":{}", json_string(u)));
    }
    if !self.files.is_empty() {
      let files: Vec<String> =
        self.files.iter().map(|f| json_string(f)).collect();
      parts.push(format!("\"files\":[{}]", files.join(",")));
    }
    if let Some((x, y)) = self.point {
      parts.push(format!("\"x\":{x},\"y\":{y}"));
    }
    format!("{{{}}}", parts.join(","))
  }
}

pub const LAUFEY_SHARE_COMPLETED: c_int = 0;
pub const LAUFEY_SHARE_SHOWN: c_int = 1;
pub const LAUFEY_SHARE_CANCELLED: c_int = 2;
pub const LAUFEY_SHARE_NOT_SUPPORTED: c_int = 3;
pub const LAUFEY_SHARE_FAILED: c_int = 4;

fn outcome(status: c_int, detail: Option<String>) -> ShareOutcome {
  match status {
    LAUFEY_SHARE_COMPLETED => ShareOutcome::Completed(detail),
    LAUFEY_SHARE_SHOWN => ShareOutcome::Shown,
    LAUFEY_SHARE_CANCELLED => ShareOutcome::Cancelled,
    LAUFEY_SHARE_NOT_SUPPORTED => ShareOutcome::NotSupported(detail),
    _ => ShareOutcome::Failed(detail),
  }
}

type ShareCallback = Box<dyn FnOnce(ShareOutcome) + Send>;

unsafe extern "C" fn share_trampoline(
  user_data: *mut c_void,
  status: c_int,
  detail: *const c_char,
) {
  if user_data.is_null() {
    return;
  }
  // SAFETY: the box made in share_with; the backend fires it once.
  let cb = unsafe { Box::from_raw(user_data as *mut ShareCallback) };
  let detail = (!detail.is_null()).then(|| {
    unsafe { CStr::from_ptr(detail) }
      .to_string_lossy()
      .into_owned()
  });
  cb(outcome(status, detail));
}

/// Show the OS share sheet over window `window_id` (0: the app's key /
/// foreground window). `callback` runs exactly once.
pub fn share<F>(window_id: u32, request: &ShareRequest, callback: F)
where
  F: FnOnce(ShareOutcome) + Send + 'static,
{
  share_with(api(), window_id, request, callback)
}

fn share_with<F>(
  api: &LaufeyBackendApi,
  window_id: u32,
  request: &ShareRequest,
  callback: F,
) where
  F: FnOnce(ShareOutcome) + Send + 'static,
{
  let Some(f) = api.share else {
    callback(ShareOutcome::NotSupported(None));
    return;
  };
  let Ok(json) = CString::new(request.to_json()) else {
    callback(ShareOutcome::Failed(Some(
      "the request contains a NUL byte".into(),
    )));
    return;
  };
  let boxed: Box<ShareCallback> = Box::new(Box::new(callback));
  unsafe {
    f(
      api.backend_data,
      window_id,
      json.as_ptr(),
      Some(share_trampoline),
      Box::into_raw(boxed) as *mut c_void,
    )
  };
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::sync::Mutex;

  static LAST: Mutex<String> = Mutex::new(String::new());

  unsafe extern "C" fn fake_share(
    _: *mut c_void,
    _window: u32,
    json: *const c_char,
    cb: Option<unsafe extern "C" fn(*mut c_void, c_int, *const c_char)>,
    ud: *mut c_void,
  ) {
    *LAST.lock().unwrap() = unsafe { CStr::from_ptr(json) }
      .to_string_lossy()
      .into_owned();
    let name = CString::new("Mail").unwrap();
    unsafe { cb.unwrap()(ud, LAUFEY_SHARE_COMPLETED, name.as_ptr()) };
  }

  #[test]
  fn not_supported_without_the_entry_point() {
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    let (tx, rx) = std::sync::mpsc::channel();
    share_with(&none, 0, &ShareRequest::default(), move |o| {
      let _ = tx.send(o);
    });
    assert_eq!(rx.recv().unwrap(), ShareOutcome::NotSupported(None));
  }

  #[test]
  fn sends_the_request_and_reports_the_outcome() {
    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    api.share = Some(fake_share);
    let request = ShareRequest {
      title: Some("T".into()),
      text: Some("a \"quote\"\n".into()),
      url: Some("https://a.example/".into()),
      files: vec!["/tmp/x".into()],
      point: Some((3, 4)),
    };
    let (tx, rx) = std::sync::mpsc::channel();
    share_with(&api, 7, &request, move |o| {
      let _ = tx.send(o);
    });
    assert_eq!(
      rx.recv().unwrap(),
      ShareOutcome::Completed(Some("Mail".into()))
    );
    assert_eq!(
      *LAST.lock().unwrap(),
      r#"{"title":"T","text":"a \"quote\"\u000a","url":"https://a.example/","files":["/tmp/x"],"x":3,"y":4}"#
    );
  }

  #[test]
  fn outcomes() {
    assert_eq!(outcome(LAUFEY_SHARE_SHOWN, None), ShareOutcome::Shown);
    assert_eq!(
      outcome(LAUFEY_SHARE_CANCELLED, None),
      ShareOutcome::Cancelled
    );
    assert_eq!(
      outcome(99, Some("x".into())),
      ShareOutcome::Failed(Some("x".into()))
    );
  }
}
