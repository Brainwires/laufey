// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Application-menu clicks for the app (API 55): one app-level handler hears
//! every click on a custom item of an application menu
//! ([`crate::Window::set_menu`]), whether or not the window the menu was set
//! for is still open. The macOS menu bar keeps showing the menu of the last
//! window that set one, so an app whose windows have all closed (a tray app,
//! one kept alive) still gets its menu's clicks here. See `docs/menus.md`.
//!
//! A backend with `set_app_menu_click_handler` (CEF, WebView) reports the
//! clicks itself; on one without it (Winit) the clicks are taken from the
//! window menus' own callback, so the handler works on every backend.

use std::ffi::{c_char, c_void, CStr};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

use crate::{api, LaufeyBackendApi};

type AppMenuClickHandler = Arc<dyn Fn(u32, &str) + Send + Sync>;

static APP_MENU_CLICK_HANDLER: Mutex<Option<AppMenuClickHandler>> =
  Mutex::new(None);

/// Whether the backend reports the clicks to [`app_menu_click_callback`]
/// itself, so the window menus' callback must not report them again.
static REPORTED_BY_BACKEND: AtomicBool = AtomicBool::new(false);

/// Sets the app-level application-menu click handler, replacing any other:
/// `handler(window_id, item_id)` runs for every click on a custom item of
/// an application menu, after the window's own `set_menu` handler, with the
/// id of the window the menu was set for (which may have closed since). Runs
/// on the backend's UI thread.
pub fn set_app_menu_click_handler<F>(handler: F)
where
  F: Fn(u32, &str) + Send + Sync + 'static,
{
  set_app_menu_click_handler_with(api(), Some(Arc::new(handler)));
}

/// Removes the app-level application-menu click handler.
pub fn clear_app_menu_click_handler() {
  set_app_menu_click_handler_with(api(), None);
}

fn set_app_menu_click_handler_with(
  api: &LaufeyBackendApi,
  handler: Option<AppMenuClickHandler>,
) {
  let install = handler.is_some();
  *APP_MENU_CLICK_HANDLER.lock().unwrap() = handler;
  match api.set_app_menu_click_handler {
    Some(f) => {
      unsafe {
        f(
          api.backend_data,
          if install {
            Some(app_menu_click_callback)
          } else {
            None
          },
          std::ptr::null_mut(),
        );
      }
      REPORTED_BY_BACKEND.store(install, Ordering::SeqCst);
    }
    None => REPORTED_BY_BACKEND.store(false, Ordering::SeqCst),
  }
}

fn run_app_menu_click_handler(window_id: u32, item_id: &str) {
  // Cloned out: the handler runs without the lock held (it may replace
  // itself).
  let handler = APP_MENU_CLICK_HANDLER.lock().unwrap().clone();
  if let Some(handler) = handler {
    handler(window_id, item_id);
  }
}

/// The backend's report (`set_app_menu_click_handler`).
unsafe extern "C" fn app_menu_click_callback(
  _user_data: *mut c_void,
  window_id: u32,
  item_id: *const c_char,
) {
  if item_id.is_null() {
    return;
  }
  let id = unsafe { CStr::from_ptr(item_id) }.to_string_lossy();
  run_app_menu_click_handler(window_id, &id);
}

/// A click the window menus' own callback received: reported to the app
/// handler here unless the backend reports it itself.
pub(crate) fn report_window_menu_click(window_id: u32, item_id: &str) {
  if !REPORTED_BY_BACKEND.load(Ordering::SeqCst) {
    run_app_menu_click_handler(window_id, item_id);
  }
}

#[cfg(test)]
mod tests {
  use std::ffi::CString;

  use super::*;

  // The handler is process-wide: the tests take turns.
  static SERIAL: Mutex<()> = Mutex::new(());

  type ClickFn = unsafe extern "C" fn(*mut c_void, u32, *const c_char);

  // What the fake backend was last handed (as usize: raw pointers aren't
  // Send).
  static REGISTERED: Mutex<Option<Option<usize>>> = Mutex::new(None);

  unsafe extern "C" fn fake_set_handler(
    _: *mut c_void,
    handler: Option<ClickFn>,
    _: *mut c_void,
  ) {
    *REGISTERED.lock().unwrap() = Some(handler.map(|f| f as usize));
  }

  fn registered() -> Option<ClickFn> {
    REGISTERED
      .lock()
      .unwrap()
      .unwrap()
      .map(|f| unsafe { std::mem::transmute::<usize, ClickFn>(f) })
  }

  fn with_backend_entry() -> LaufeyBackendApi {
    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    api.set_app_menu_click_handler = Some(fake_set_handler);
    api
  }

  fn recorder() -> (Arc<Mutex<Vec<(u32, String)>>>, AppMenuClickHandler) {
    let seen = Arc::new(Mutex::new(Vec::new()));
    let sink = seen.clone();
    let handler: AppMenuClickHandler = Arc::new(move |window_id, id: &str| {
      // lib.rs's handlers_run_without_their_slot_lock clicks window 77's
      // menu from another thread, unaware of this handler.
      if window_id != 77 {
        sink.lock().unwrap().push((window_id, id.to_string()));
      }
    });
    (seen, handler)
  }

  #[test]
  fn a_backend_with_the_entry_reports_each_click_once() {
    let _serial = SERIAL.lock().unwrap();
    let api = with_backend_entry();
    let (seen, handler) = recorder();
    set_app_menu_click_handler_with(&api, Some(handler));
    let report = registered().expect("the backend got a handler");

    // The backend's report reaches the handler, with the menu's window id
    // (here one that has closed).
    let id = CString::new("open-recent").unwrap();
    unsafe { report(std::ptr::null_mut(), 4, id.as_ptr()) };
    unsafe { report(std::ptr::null_mut(), 4, std::ptr::null()) };
    // The window menus' callback doesn't report it a second time.
    report_window_menu_click(4, "open-recent");
    assert_eq!(*seen.lock().unwrap(), [(4, "open-recent".to_string())]);

    // Cleared: the backend is told, and nothing is reported any more.
    set_app_menu_click_handler_with(&api, None);
    assert!(registered().is_none());
    unsafe { report(std::ptr::null_mut(), 4, id.as_ptr()) };
    report_window_menu_click(4, "open-recent");
    assert_eq!(seen.lock().unwrap().len(), 1);
  }

  #[test]
  fn a_backend_without_the_entry_reports_from_the_window_menus() {
    let _serial = SERIAL.lock().unwrap();
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    let (seen, handler) = recorder();
    set_app_menu_click_handler_with(&none, Some(handler));
    report_window_menu_click(1, "quit");
    report_window_menu_click(9, "about");
    assert_eq!(
      *seen.lock().unwrap(),
      [(1, "quit".to_string()), (9, "about".to_string())]
    );
    set_app_menu_click_handler_with(&none, None);
    report_window_menu_click(1, "quit");
    assert_eq!(seen.lock().unwrap().len(), 2);
  }

  #[test]
  fn a_handler_may_replace_itself() {
    let _serial = SERIAL.lock().unwrap();
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    let (seen, second) = recorder();
    let first: AppMenuClickHandler = Arc::new(move |_, _: &str| {
      *APP_MENU_CLICK_HANDLER.lock().unwrap() = Some(second.clone());
    });
    set_app_menu_click_handler_with(&none, Some(first));
    report_window_menu_click(2, "a");
    report_window_menu_click(2, "b");
    assert_eq!(*seen.lock().unwrap(), [(2, "b".to_string())]);
    set_app_menu_click_handler_with(&none, None);
  }
}
