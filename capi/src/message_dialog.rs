// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Native message dialogs (API 54): the OS's own message box with the app's
//! button labels. See `docs/message-dialogs.md`. Any thread; the request is
//! made when [`show_message_dialog`] is called, and the future resolves when
//! the user closes the dialog.

use std::ffi::{c_char, c_int, c_void, CString};
use std::future::Future;

use crate::{api, LaufeyBackendApi};

// --- Constants (mirror laufey.h) -------------------------------------------

pub const LAUFEY_MESSAGE_DIALOG_INFO: i32 = 0;
pub const LAUFEY_MESSAGE_DIALOG_WARNING: i32 = 1;
pub const LAUFEY_MESSAGE_DIALOG_ERROR: i32 = 2;
pub const LAUFEY_MESSAGE_DIALOG_ANSWERED: i32 = 0;
pub const LAUFEY_MESSAGE_DIALOG_CANCELLED: i32 = 1;
pub const LAUFEY_MESSAGE_DIALOG_FAILED: i32 = 2;
pub const LAUFEY_MESSAGE_DIALOG_MAX_BUTTONS: usize = 16;

/// The icon a message dialog shows.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum MessageDialogKind {
  #[default]
  Info,
  Warning,
  Error,
}

impl MessageDialogKind {
  fn to_raw(self) -> i32 {
    match self {
      MessageDialogKind::Info => LAUFEY_MESSAGE_DIALOG_INFO,
      MessageDialogKind::Warning => LAUFEY_MESSAGE_DIALOG_WARNING,
      MessageDialogKind::Error => LAUFEY_MESSAGE_DIALOG_ERROR,
    }
  }
}

/// Options for [`show_message_dialog`]. `message` is the main line,
/// `detail` the smaller text under it, `title` the window title where the OS
/// shows one. `buttons` are the labels (none: one "OK"); `default_index` is
/// the button Return presses (`None`: the first), `cancel_index` the one
/// Escape presses (`None`: Escape dismisses the dialog where the OS lets it).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct MessageDialogOptions {
  pub kind: MessageDialogKind,
  pub title: Option<String>,
  pub message: String,
  pub detail: Option<String>,
  pub buttons: Vec<String>,
  pub default_index: Option<u32>,
  pub cancel_index: Option<u32>,
}

/// How a message dialog ended.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MessageDialogOutcome {
  /// The button at this index was pressed.
  Answered(u32),
  /// Dismissed without a button (Escape with no cancel button, the close
  /// button, quitting).
  Cancelled,
  /// It could not be shown (bad options, no message dialogs on this backend,
  /// no display).
  Failed,
}

/// A dialog [`show_message_dialog`] opened: its id (0 if it was answered at
/// once) and the future of its outcome.
pub struct MessageDialog<F> {
  pub id: u32,
  pub outcome: F,
}

fn outcome_from_raw(status: c_int, response: i32) -> MessageDialogOutcome {
  match status {
    LAUFEY_MESSAGE_DIALOG_ANSWERED if response >= 0 => {
      MessageDialogOutcome::Answered(response as u32)
    }
    LAUFEY_MESSAGE_DIALOG_CANCELLED => MessageDialogOutcome::Cancelled,
    _ => MessageDialogOutcome::Failed,
  }
}

unsafe extern "C" fn message_dialog_trampoline(
  user_data: *mut c_void,
  _dialog_id: u32,
  status: c_int,
  response: i32,
) {
  // Exactly once per request (laufey.h).
  let tx = unsafe {
    Box::from_raw(
      user_data as *mut tokio::sync::oneshot::Sender<MessageDialogOutcome>,
    )
  };
  let _ = tx.send(outcome_from_raw(status, response));
}

/// Show the OS's message box, modal to window `window_id` (a sheet on macOS)
/// or app-modal for 0, which works with no window open. Never blocks: the
/// outcome future resolves when the user closes it. Several may be open at
/// once. `Failed` on a backend without message dialogs
/// (`WindowCapabilities::message_dialogs`).
pub fn show_message_dialog(
  window_id: u32,
  options: &MessageDialogOptions,
) -> MessageDialog<impl Future<Output = MessageDialogOutcome> + Send + 'static>
{
  show_message_dialog_with(api(), window_id, options)
}

fn index_to_raw(index: Option<u32>) -> i32 {
  match index {
    Some(i) if i <= i32::MAX as u32 => i as i32,
    _ => -1,
  }
}

fn show_message_dialog_with(
  api: &LaufeyBackendApi,
  window_id: u32,
  options: &MessageDialogOptions,
) -> MessageDialog<impl Future<Output = MessageDialogOutcome> + Send + 'static>
{
  struct COptions {
    title: Option<CString>,
    message: CString,
    detail: Option<CString>,
    buttons: Vec<CString>,
  }
  let opt = |s: &Option<String>| -> Result<Option<CString>, ()> {
    match s {
      None => Ok(None),
      Some(v) => CString::new(v.as_str()).map(Some).map_err(|_| ()),
    }
  };
  let built: Result<COptions, ()> = (|| {
    if options.buttons.len() > LAUFEY_MESSAGE_DIALOG_MAX_BUTTONS {
      return Err(());
    }
    Ok(COptions {
      title: opt(&options.title)?,
      message: CString::new(options.message.as_str()).map_err(|_| ())?,
      detail: opt(&options.detail)?,
      buttons: options
        .buttons
        .iter()
        .map(|b| CString::new(b.as_str()).map_err(|_| ()))
        .collect::<Result<Vec<_>, _>>()?,
    })
  })();
  let mut id = 0;
  let rx = match (api.show_message_dialog, built) {
    (Some(f), Ok(c)) => {
      let button_ptrs: Vec<*const c_char> =
        c.buttons.iter().map(|b| b.as_ptr()).collect();
      let raw = crate::ffi::laufey_message_dialog_options_t {
        kind: options.kind.to_raw(),
        title: c.title.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        message: c.message.as_ptr(),
        detail: c.detail.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
        buttons: if button_ptrs.is_empty() {
          std::ptr::null()
        } else {
          button_ptrs.as_ptr()
        },
        button_count: button_ptrs.len(),
        default_index: index_to_raw(options.default_index),
        cancel_index: index_to_raw(options.cancel_index),
      };
      let (tx, rx) = tokio::sync::oneshot::channel::<MessageDialogOutcome>();
      let user_data = Box::into_raw(Box::new(tx)) as *mut c_void;
      id = unsafe {
        f(
          api.backend_data,
          window_id,
          &raw,
          Some(message_dialog_trampoline),
          user_data,
        )
      };
      Some(rx)
    }
    _ => None,
  };
  MessageDialog {
    id,
    outcome: async move {
      match rx {
        Some(rx) => rx.await.unwrap_or(MessageDialogOutcome::Failed),
        None => MessageDialogOutcome::Failed,
      }
    },
  }
}

#[cfg(test)]
mod tests {
  use std::ffi::CStr;

  use super::*;

  fn block_on<F: Future>(fut: F) -> F::Output {
    tokio::runtime::Builder::new_current_thread()
      .build()
      .unwrap()
      .block_on(fut)
  }

  // A backend that answers from the options it was given: the message names
  // the outcome, so each case checks the options arrived intact.
  unsafe extern "C" fn fake_show(
    _: *mut c_void,
    window_id: u32,
    options: *const crate::ffi::laufey_message_dialog_options_t,
    cb: Option<unsafe extern "C" fn(*mut c_void, u32, c_int, i32)>,
    ud: *mut c_void,
  ) -> u32 {
    let cb = cb.unwrap();
    let o = unsafe { &*options };
    let message = unsafe { CStr::from_ptr(o.message) }.to_str().unwrap();
    match message {
      "pick" => {
        assert_eq!(window_id, 3);
        assert_eq!(o.kind, LAUFEY_MESSAGE_DIALOG_WARNING);
        assert_eq!(o.button_count, 3);
        let labels: Vec<&str> = (0..o.button_count)
          .map(|i| {
            unsafe { CStr::from_ptr(*o.buttons.add(i)) }
              .to_str()
              .unwrap()
          })
          .collect();
        assert_eq!(labels, ["Save", "Don't Save", "Cancel"]);
        assert_eq!(o.default_index, 0);
        assert_eq!(o.cancel_index, 2);
        let detail = unsafe { CStr::from_ptr(o.detail) }.to_str().unwrap();
        assert_eq!(detail, "Unsaved changes");
        assert!(o.title.is_null());
        unsafe { cb(ud, 7, LAUFEY_MESSAGE_DIALOG_ANSWERED, 1) };
        7
      }
      "dismiss" => {
        assert_eq!(window_id, 0);
        assert!(o.buttons.is_null());
        assert_eq!(o.button_count, 0);
        assert_eq!((o.default_index, o.cancel_index), (-1, -1));
        unsafe { cb(ud, 8, LAUFEY_MESSAGE_DIALOG_CANCELLED, -1) };
        8
      }
      _ => {
        unsafe { cb(ud, 0, LAUFEY_MESSAGE_DIALOG_FAILED, -1) };
        0
      }
    }
  }

  fn pick() -> MessageDialogOptions {
    MessageDialogOptions {
      kind: MessageDialogKind::Warning,
      message: "pick".into(),
      detail: Some("Unsaved changes".into()),
      buttons: vec!["Save".into(), "Don't Save".into(), "Cancel".into()],
      default_index: Some(0),
      cancel_index: Some(2),
      ..Default::default()
    }
  }

  #[test]
  fn a_backend_without_message_dialogs_fails() {
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    let d = show_message_dialog_with(&none, 3, &pick());
    assert_eq!(d.id, 0);
    assert_eq!(block_on(d.outcome), MessageDialogOutcome::Failed);
  }

  #[test]
  fn message_dialog_outcomes() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    fake.show_message_dialog = Some(fake_show);
    let d = show_message_dialog_with(&fake, 3, &pick());
    assert_eq!(d.id, 7);
    assert_eq!(block_on(d.outcome), MessageDialogOutcome::Answered(1));
    let dismiss = MessageDialogOptions {
      message: "dismiss".into(),
      ..Default::default()
    };
    let d = show_message_dialog_with(&fake, 0, &dismiss);
    assert_eq!(
      (d.id, block_on(d.outcome)),
      (8, MessageDialogOutcome::Cancelled)
    );
    let other = MessageDialogOptions {
      message: "other".into(),
      ..Default::default()
    };
    let d = show_message_dialog_with(&fake, 0, &other);
    assert_eq!(
      (d.id, block_on(d.outcome)),
      (0, MessageDialogOutcome::Failed)
    );
  }

  #[test]
  fn invalid_options_never_reach_the_backend() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    fake.show_message_dialog = Some(fake_show);
    let mut nul = pick();
    nul.message = "pi\0ck".into();
    let d = show_message_dialog_with(&fake, 3, &nul);
    assert_eq!(
      (d.id, block_on(d.outcome)),
      (0, MessageDialogOutcome::Failed)
    );
    let mut many = pick();
    many.buttons = vec!["b".into(); LAUFEY_MESSAGE_DIALOG_MAX_BUTTONS + 1];
    let d = show_message_dialog_with(&fake, 3, &many);
    assert_eq!(
      (d.id, block_on(d.outcome)),
      (0, MessageDialogOutcome::Failed)
    );
  }

  #[test]
  fn constants_match_the_header() {
    use crate::ffi;
    assert_eq!(
      LAUFEY_MESSAGE_DIALOG_INFO,
      ffi::LAUFEY_MESSAGE_DIALOG_INFO as i32
    );
    assert_eq!(
      LAUFEY_MESSAGE_DIALOG_WARNING,
      ffi::LAUFEY_MESSAGE_DIALOG_WARNING as i32
    );
    assert_eq!(
      LAUFEY_MESSAGE_DIALOG_ERROR,
      ffi::LAUFEY_MESSAGE_DIALOG_ERROR as i32
    );
    assert_eq!(
      LAUFEY_MESSAGE_DIALOG_ANSWERED,
      ffi::LAUFEY_MESSAGE_DIALOG_ANSWERED as i32
    );
    assert_eq!(
      LAUFEY_MESSAGE_DIALOG_CANCELLED,
      ffi::LAUFEY_MESSAGE_DIALOG_CANCELLED as i32
    );
    assert_eq!(
      LAUFEY_MESSAGE_DIALOG_FAILED,
      ffi::LAUFEY_MESSAGE_DIALOG_FAILED as i32
    );
    assert_eq!(
      LAUFEY_MESSAGE_DIALOG_MAX_BUTTONS,
      ffi::LAUFEY_MESSAGE_DIALOG_MAX_BUTTONS as usize
    );
  }

  #[test]
  fn raw_outcomes_decode() {
    assert_eq!(
      outcome_from_raw(LAUFEY_MESSAGE_DIALOG_ANSWERED, 2),
      MessageDialogOutcome::Answered(2)
    );
    // An answer with no button is not an answer.
    assert_eq!(
      outcome_from_raw(LAUFEY_MESSAGE_DIALOG_ANSWERED, -1),
      MessageDialogOutcome::Failed
    );
    assert_eq!(
      outcome_from_raw(LAUFEY_MESSAGE_DIALOG_CANCELLED, -1),
      MessageDialogOutcome::Cancelled
    );
    assert_eq!(outcome_from_raw(99, 0), MessageDialogOutcome::Failed);
    assert_eq!(index_to_raw(None), -1);
    assert_eq!(index_to_raw(Some(4)), 4);
    assert_eq!(index_to_raw(Some(u32::MAX)), -1);
  }
}
