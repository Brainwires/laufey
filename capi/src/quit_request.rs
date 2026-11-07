// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The OS asks the app to quit (API 52). See `docs/c-abi.md`
//! ("Quit requests").

use std::ffi::c_void;
use std::sync::{Arc, Mutex, OnceLock};

use crate::{api, lock_unpoisoned, LaufeyBackendApi};

/// Why the OS asks the app to quit.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum QuitRequestKind {
  /// The user quit the app (Cmd+Q, the Dock's Quit, a quit Apple Event).
  App,
  /// The session is ending (logout, restart, shutdown).
  SessionEnd,
}

impl QuitRequestKind {
  fn from_raw(kind: i32) -> Self {
    if kind == crate::ffi::LAUFEY_QUIT_REQUEST_SESSION_END as i32 {
      QuitRequestKind::SessionEnd
    } else {
      QuitRequestKind::App
    }
  }
}

type Handler = Arc<dyn Fn(QuitRequestKind) + Send + Sync>;

fn handler_slot() -> &'static Mutex<Option<Handler>> {
  static SLOT: OnceLock<Mutex<Option<Handler>>> = OnceLock::new();
  SLOT.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn trampoline(kind: i32, _user_data: *mut c_void) {
  let handler = lock_unpoisoned(handler_slot()).clone();
  if let Some(handler) = handler {
    handler(QuitRequestKind::from_raw(kind));
  }
}

pub(crate) fn on_quit_requested_with<F>(api: &LaufeyBackendApi, handler: F)
where
  F: Fn(QuitRequestKind) + Send + Sync + 'static,
{
  *lock_unpoisoned(handler_slot()) = Some(Arc::new(handler));
  if let Some(f) = api.set_quit_requested_handler {
    unsafe {
      f(api.backend_data, Some(trampoline), std::ptr::null_mut());
    }
  }
}

/// Register the (single, process-wide) handler fired when the OS asks the app
/// to quit: on macOS `terminate:` (Cmd+Q, the Dock, a logout), on Windows
/// `WM_QUERYENDSESSION`. Fires on the UI thread. The OS waits for
/// [`answer_quit_request`] or [`quit`](crate::quit), but never past a
/// deadline, when the quit goes ahead. Replaces any previous handler.
pub fn on_quit_requested<F>(handler: F)
where
  F: Fn(QuitRequestKind) + Send + Sync + 'static,
{
  on_quit_requested_with(api(), handler);
}

/// Answer the quit request in progress: `false` keeps the app running (and
/// asks the OS to stop a logout, which it may not honour). No effect when none
/// is. Any thread.
pub fn answer_quit_request(allow: bool) {
  let api = api();
  if let Some(f) = api.answer_quit_request {
    unsafe { f(api.backend_data, allow) };
  }
}

/// Whether a quit request is still waiting for its answer: `false` once it was
/// answered or its deadline passed. Any thread.
pub fn quit_request_pending() -> bool {
  let api = api();
  match api.quit_request_pending {
    Some(f) => unsafe { f(api.backend_data) },
    None => false,
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn the_kind_decodes() {
    assert_eq!(
      QuitRequestKind::from_raw(crate::ffi::LAUFEY_QUIT_REQUEST_APP as i32),
      QuitRequestKind::App
    );
    assert_eq!(
      QuitRequestKind::from_raw(
        crate::ffi::LAUFEY_QUIT_REQUEST_SESSION_END as i32
      ),
      QuitRequestKind::SessionEnd
    );
    // A kind this crate doesn't know is a plain quit.
    assert_eq!(QuitRequestKind::from_raw(77), QuitRequestKind::App);
  }

  #[test]
  fn a_backend_without_the_entry_point_is_left_alone() {
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    on_quit_requested_with(&none, |_| {});
  }
}
