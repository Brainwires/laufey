// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Will-navigate (API 56): every top-level navigation of a window is reported
//! to one handler before the navigation guard decides it, and waits for
//! [`answer_will_navigate`]. See `set_will_navigate_handler` in `laufey.h`
//! and `docs/window-management.md`.

use std::ffi::{c_char, c_void, CStr};
use std::sync::{Arc, Mutex, OnceLock};

use crate::{api, LaufeyBackendApi};

/// How a top-level navigation started (`LAUFEY_NAVIGATION_KIND_*`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum NavigationKind {
  /// A form, a typed URL, anything else (and what an engine can't tell).
  Other,
  /// A link the user activated.
  Link,
  /// A script (`location = …`).
  Script,
  /// A server or meta redirect.
  Redirect,
  /// `window.open` / `target="_blank"` (reserved: not reported yet).
  WindowOpen,
}

impl NavigationKind {
  fn from_raw(kind: u32) -> Self {
    match kind {
      1 => NavigationKind::Link,
      2 => NavigationKind::Script,
      3 => NavigationKind::Redirect,
      4 => NavigationKind::WindowOpen,
      _ => NavigationKind::Other,
    }
  }

  /// The name the runtime's event carries: `"link"`, `"script"`,
  /// `"redirect"`, `"window-open"` or `"other"`.
  pub fn as_str(self) -> &'static str {
    match self {
      NavigationKind::Other => "other",
      NavigationKind::Link => "link",
      NavigationKind::Script => "script",
      NavigationKind::Redirect => "redirect",
      NavigationKind::WindowOpen => "window-open",
    }
  }
}

/// The embedder's decision on a reported navigation
/// (`LAUFEY_NAVIGATION_DEFAULT` / `_ALLOW` / `_REFUSE`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum NavigationAnswer {
  /// The navigation guard decides, as with no handler.
  Default,
  /// Load it in the window, whatever the guard would say.
  Allow,
  /// Cancel it; nothing opens in the browser either.
  Refuse,
}

impl NavigationAnswer {
  fn to_raw(self) -> u32 {
    match self {
      NavigationAnswer::Default => 0,
      NavigationAnswer::Allow => 1,
      NavigationAnswer::Refuse => 2,
    }
  }
}

/// A top-level navigation waiting for [`answer_will_navigate`].
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WillNavigate {
  /// The window that is about to navigate.
  pub window_id: u32,
  /// Names the navigation for [`answer_will_navigate`].
  pub id: u64,
  /// Where it goes.
  pub url: String,
  /// The URL of the document that started it, when the engine says.
  pub initiator: Option<String>,
  /// How it started.
  pub kind: NavigationKind,
}

type Handler = Arc<dyn Fn(WillNavigate) + Send + Sync>;

fn handler_slot() -> &'static Mutex<Option<Handler>> {
  static SLOT: OnceLock<Mutex<Option<Handler>>> = OnceLock::new();
  SLOT.get_or_init(|| Mutex::new(None))
}

/// Decodes the backend's arguments; `None` without a URL (or one that is
/// not UTF-8). An initiator that is not UTF-8 is dropped.
fn navigation_from_raw(
  window_id: u32,
  request_id: u64,
  url: *const c_char,
  initiator: *const c_char,
  kind: u32,
) -> Option<WillNavigate> {
  if url.is_null() {
    return None;
  }
  let url = unsafe { CStr::from_ptr(url) }.to_str().ok()?.to_string();
  let initiator = if initiator.is_null() {
    None
  } else {
    unsafe { CStr::from_ptr(initiator) }
      .to_str()
      .ok()
      .map(str::to_string)
  };
  Some(WillNavigate {
    window_id,
    id: request_id,
    url,
    initiator,
    kind: NavigationKind::from_raw(kind),
  })
}

fn deliver(
  api: &LaufeyBackendApi,
  window_id: u32,
  request_id: u64,
  url: *const c_char,
  initiator: *const c_char,
  kind: u32,
) {
  let handler = handler_slot().lock().unwrap().clone();
  match (
    navigation_from_raw(window_id, request_id, url, initiator, kind),
    handler,
  ) {
    (Some(navigation), Some(handler)) => handler(navigation),
    // Nobody can answer it: the guard decides now.
    _ => {
      answer_will_navigate_with(api, request_id, NavigationAnswer::Default);
    }
  }
}

unsafe extern "C" fn trampoline(
  _user_data: *mut c_void,
  window_id: u32,
  request_id: u64,
  url: *const c_char,
  initiator: *const c_char,
  kind: u32,
) {
  deliver(api(), window_id, request_id, url, initiator, kind);
}

pub(crate) fn set_will_navigate_handler_with<F>(
  api: &LaufeyBackendApi,
  handler: F,
) -> bool
where
  F: Fn(WillNavigate) + Send + Sync + 'static,
{
  let Some(f) = api.set_will_navigate_handler else {
    return false;
  };
  *handler_slot().lock().unwrap() = Some(Arc::new(handler));
  unsafe { f(api.backend_data, Some(trampoline), std::ptr::null_mut()) };
  true
}

/// Register the (single, process-wide) will-navigate handler. It fires on
/// the UI thread for each top-level navigation a page starts, before the
/// navigation guard; answer each with [`answer_will_navigate`] (any thread),
/// or the guard decides after 10 seconds. Replaces any previous handler.
/// On CEF and WebView2, which cancel a navigation and start it again, a
/// non-GET one (a form POST) is not reported: the guard decides it at once.
/// Returns `false` when the backend has no such hook (Winit, or older than
/// API 56).
pub fn set_will_navigate_handler<F>(handler: F) -> bool
where
  F: Fn(WillNavigate) + Send + Sync + 'static,
{
  set_will_navigate_handler_with(api(), handler)
}

pub(crate) fn clear_will_navigate_handler_with(api: &LaufeyBackendApi) {
  *handler_slot().lock().unwrap() = None;
  if let Some(f) = api.set_will_navigate_handler {
    unsafe { f(api.backend_data, None, std::ptr::null_mut()) };
  }
}

/// Remove the handler: what still waits goes to the guard, and nothing is
/// reported any more.
pub fn clear_will_navigate_handler() {
  clear_will_navigate_handler_with(api());
}

fn answer_will_navigate_with(
  api: &LaufeyBackendApi,
  id: u64,
  answer: NavigationAnswer,
) -> bool {
  match api.answer_will_navigate {
    Some(f) => unsafe { f(api.backend_data, id, answer.to_raw()) },
    None => false,
  }
}

/// Answer navigation `id`. Returns `false` when it is not waiting (answered,
/// timed out, its window closed, or never reported). Any thread.
pub fn answer_will_navigate(id: u64, answer: NavigationAnswer) -> bool {
  answer_will_navigate_with(api(), id, answer)
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::ffi::CString;
  use std::sync::atomic::{AtomicBool, Ordering};

  static ANSWERS: Mutex<Vec<(u64, u32)>> = Mutex::new(Vec::new());
  static HANDLER_SET: AtomicBool = AtomicBool::new(false);
  // The slot is process-wide; the tests that use it take turns.
  static SERIAL: Mutex<()> = Mutex::new(());

  type RawHandler = unsafe extern "C" fn(
    *mut c_void,
    u32,
    u64,
    *const c_char,
    *const c_char,
    u32,
  );

  unsafe extern "C" fn fake_set(
    _: *mut c_void,
    handler: Option<RawHandler>,
    _: *mut c_void,
  ) {
    HANDLER_SET.store(handler.is_some(), Ordering::SeqCst);
  }

  unsafe extern "C" fn fake_answer(_: *mut c_void, id: u64, d: u32) -> bool {
    ANSWERS.lock().unwrap().push((id, d));
    true
  }

  fn fake_api() -> LaufeyBackendApi {
    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    api.set_will_navigate_handler = Some(fake_set);
    api.answer_will_navigate = Some(fake_answer);
    api
  }

  #[test]
  fn kinds_and_answers_match_the_header() {
    assert_eq!(NavigationKind::from_raw(0), NavigationKind::Other);
    assert_eq!(NavigationKind::from_raw(1), NavigationKind::Link);
    assert_eq!(NavigationKind::from_raw(2), NavigationKind::Script);
    assert_eq!(NavigationKind::from_raw(3), NavigationKind::Redirect);
    assert_eq!(NavigationKind::from_raw(4), NavigationKind::WindowOpen);
    assert_eq!(NavigationKind::from_raw(99), NavigationKind::Other);
    assert_eq!(NavigationKind::WindowOpen.as_str(), "window-open");
    assert_eq!(
      NavigationKind::from_raw(crate::ffi::LAUFEY_NAVIGATION_KIND_REDIRECT),
      NavigationKind::Redirect
    );
    assert_eq!(
      NavigationAnswer::Default.to_raw(),
      crate::ffi::LAUFEY_NAVIGATION_DEFAULT
    );
    assert_eq!(
      NavigationAnswer::Allow.to_raw(),
      crate::ffi::LAUFEY_NAVIGATION_ALLOW
    );
    assert_eq!(
      NavigationAnswer::Refuse.to_raw(),
      crate::ffi::LAUFEY_NAVIGATION_REFUSE
    );
  }

  #[test]
  fn a_navigation_decodes() {
    let url = CString::new("https://example.com/").unwrap();
    let from = CString::new("myapp://app/").unwrap();
    assert_eq!(
      navigation_from_raw(2, 7, url.as_ptr(), from.as_ptr(), 1),
      Some(WillNavigate {
        window_id: 2,
        id: 7,
        url: "https://example.com/".into(),
        initiator: Some("myapp://app/".into()),
        kind: NavigationKind::Link,
      })
    );
    let n =
      navigation_from_raw(2, 7, url.as_ptr(), std::ptr::null(), 3).unwrap();
    assert_eq!(n.initiator, None);
    assert_eq!(n.kind, NavigationKind::Redirect);
    assert_eq!(
      navigation_from_raw(2, 7, std::ptr::null(), std::ptr::null(), 0),
      None
    );
  }

  #[test]
  fn not_supported_without_the_entry_points() {
    let _serial = SERIAL.lock().unwrap();
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert!(!set_will_navigate_handler_with(&none, |_| {}));
    assert!(!answer_will_navigate_with(
      &none,
      1,
      NavigationAnswer::Allow
    ));
    clear_will_navigate_handler_with(&none);
  }

  #[test]
  fn the_handler_is_asked_answered_and_cleared() {
    let _serial = SERIAL.lock().unwrap();
    let api = fake_api();
    ANSWERS.lock().unwrap().clear();
    let seen = Arc::new(Mutex::new(Vec::new()));
    let sink = seen.clone();
    assert!(set_will_navigate_handler_with(&api, move |n| {
      sink.lock().unwrap().push(n);
    }));
    assert!(HANDLER_SET.load(Ordering::SeqCst));
    let url = CString::new("myapp://app/next").unwrap();
    deliver(&api, 1, 5, url.as_ptr(), std::ptr::null(), 2);
    assert_eq!(seen.lock().unwrap().len(), 1);
    assert_eq!(seen.lock().unwrap()[0].kind, NavigationKind::Script);
    assert!(answer_will_navigate_with(&api, 5, NavigationAnswer::Refuse));
    assert_eq!(*ANSWERS.lock().unwrap(), [(5, 2)]);

    // An undecodable report goes to the guard at once.
    deliver(&api, 1, 6, std::ptr::null(), std::ptr::null(), 0);
    assert_eq!(ANSWERS.lock().unwrap().last(), Some(&(6, 0)));

    clear_will_navigate_handler_with(&api);
    assert!(!HANDLER_SET.load(Ordering::SeqCst));
    // No handler: the guard decides.
    deliver(&api, 1, 8, url.as_ptr(), std::ptr::null(), 1);
    assert_eq!(ANSWERS.lock().unwrap().last(), Some(&(8, 0)));
    assert_eq!(seen.lock().unwrap().len(), 1);
  }
}
