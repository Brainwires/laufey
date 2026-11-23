//! The host's state: the windows, the runtime's event handlers, and how the app ends.
//!
//! Window ids are laufey's (`u32`, from 1, never reused). The window map lives on the UI
//! thread; the set of live ids and the handlers are behind mutexes because the API table is
//! called from any thread.

use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::ffi::{CString, c_char, c_int, c_void};
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, AtomicU32, Ordering};

use gpui::{
  AnyWindowHandle, App, AppContext as _, Bounds, Context, TitlebarOptions,
  Window, WindowBounds, WindowHandle, WindowId, WindowOptions, px, size,
};

use crate::ui;
use crate::view::TreeView;

/// `LAUFEY_WINDOW_FLAG_*` (laufey.h).
pub const FLAG_FRAMELESS: u32 = 1 << 0;
pub const FLAG_NO_ACTIVATE: u32 = 1 << 1;
pub const FLAG_HIDDEN: u32 = 1 << 3;

/// The size of a window until the runtime sets one (the C ABI's create calls take none).
const DEFAULT_SIZE: (f32, f32) = (800.0, 600.0);

// --- Handlers ---------------------------------------------------------------------------

/// A runtime callback and its `user_data`.
#[derive(Clone, Copy)]
pub struct Handler<F: Copy> {
  pub f: F,
  pub data: usize,
}

pub type KeyboardFn = unsafe extern "C" fn(
  *mut c_void,
  u32,
  c_int,
  *const c_char,
  *const c_char,
  u32,
  bool,
);
pub type MouseClickFn =
  unsafe extern "C" fn(*mut c_void, u32, c_int, c_int, f64, f64, u32, i32);
pub type MouseMoveFn = unsafe extern "C" fn(*mut c_void, u32, f64, f64, u32);
pub type WheelFn =
  unsafe extern "C" fn(*mut c_void, u32, f64, f64, f64, f64, u32, i32);
pub type FocusedFn = unsafe extern "C" fn(*mut c_void, u32, c_int);
pub type SizeFn = unsafe extern "C" fn(*mut c_void, u32, c_int, c_int);
pub type WindowFn = unsafe extern "C" fn(*mut c_void, u32);
pub type TreeEventFn =
  unsafe extern "C" fn(*mut c_void, u32, *const c_char, usize);

/// The runtime's handlers. Each is process-wide: one per kind, for every window.
#[derive(Default)]
pub struct Handlers {
  pub keyboard: Option<Handler<KeyboardFn>>,
  pub mouse_click: Option<Handler<MouseClickFn>>,
  pub mouse_move: Option<Handler<MouseMoveFn>>,
  pub wheel: Option<Handler<WheelFn>>,
  pub focused: Option<Handler<FocusedFn>>,
  pub resize: Option<Handler<SizeFn>>,
  pub moved: Option<Handler<SizeFn>>,
  pub close_requested: Option<Handler<WindowFn>>,
  pub tree_event: Option<Handler<TreeEventFn>>,
}

static HANDLERS: Mutex<Handlers> = Mutex::new(Handlers {
  keyboard: None,
  mouse_click: None,
  mouse_move: None,
  wheel: None,
  focused: None,
  resize: None,
  moved: None,
  close_requested: None,
  tree_event: None,
});

pub fn handlers() -> std::sync::MutexGuard<'static, Handlers> {
  HANDLERS.lock().unwrap_or_else(|e| e.into_inner())
}

/// Which window-level input handlers are registered: the view only listens for what
/// somebody hears.
#[derive(Clone, Copy, Default)]
pub struct InputInterest {
  pub keyboard: bool,
  pub mouse_click: bool,
  pub mouse_move: bool,
  pub wheel: bool,
}

pub fn input_interest() -> InputInterest {
  let h = handlers();
  InputInterest {
    keyboard: h.keyboard.is_some(),
    mouse_click: h.mouse_click.is_some(),
    mouse_move: h.mouse_move.is_some(),
    wheel: h.wheel.is_some(),
  }
}

/// A handler changed: views that listen for input must render again.
pub fn refresh_windows() {
  ui::post(|app| app.refresh_windows());
}

// --- Events to the runtime --------------------------------------------------------------
//
// Each is queued as a runtime call (ui::post_call) and reads its handler when it runs, so a
// handler replaced or cleared meanwhile is honoured.

pub fn emit_tree_event(window_id: u32, json: String) {
  ui::post_call(move || {
    let handler = handlers().tree_event;
    if let Some(h) = handler {
      // SAFETY: the runtime's handler; the bytes are valid for the call.
      unsafe {
        (h.f)(
          h.data as *mut c_void,
          window_id,
          json.as_ptr().cast(),
          json.len(),
        )
      };
    }
  });
}

pub fn emit_keyboard(
  window_id: u32,
  pressed: bool,
  key: String,
  code: String,
  modifiers: u32,
  repeat: bool,
) {
  let (Ok(key), Ok(code)) = (CString::new(key), CString::new(code)) else {
    return;
  };
  ui::post_call(move || {
    let handler = handlers().keyboard;
    if let Some(h) = handler {
      // LAUFEY_KEY_PRESSED 0, LAUFEY_KEY_RELEASED 1.
      let state = if pressed { 0 } else { 1 };
      // SAFETY: the runtime's handler; the strings are valid for the call.
      unsafe {
        (h.f)(
          h.data as *mut c_void,
          window_id,
          state,
          key.as_ptr(),
          code.as_ptr(),
          modifiers,
          repeat,
        )
      };
    }
  });
}

pub fn emit_mouse_click(
  window_id: u32,
  pressed: bool,
  button: c_int,
  x: f64,
  y: f64,
  modifiers: u32,
  click_count: i32,
) {
  ui::post_call(move || {
    let handler = handlers().mouse_click;
    if let Some(h) = handler {
      let state = if pressed { 0 } else { 1 };
      // SAFETY: the runtime's handler.
      unsafe {
        (h.f)(
          h.data as *mut c_void,
          window_id,
          state,
          button,
          x,
          y,
          modifiers,
          click_count,
        )
      };
    }
  });
}

pub fn emit_mouse_move(window_id: u32, x: f64, y: f64, modifiers: u32) {
  ui::post_call(move || {
    let handler = handlers().mouse_move;
    if let Some(h) = handler {
      // SAFETY: the runtime's handler.
      unsafe { (h.f)(h.data as *mut c_void, window_id, x, y, modifiers) };
    }
  });
}

pub fn emit_wheel(
  window_id: u32,
  delta: (f64, f64),
  at: (f64, f64),
  modifiers: u32,
  delta_mode: i32,
) {
  ui::post_call(move || {
    let handler = handlers().wheel;
    if let Some(h) = handler {
      // SAFETY: the runtime's handler.
      unsafe {
        (h.f)(
          h.data as *mut c_void,
          window_id,
          delta.0,
          delta.1,
          at.0,
          at.1,
          modifiers,
          delta_mode,
        )
      };
    }
  });
}

pub fn emit_focused(window_id: u32, focused: bool) {
  ui::post_call(move || {
    let handler = handlers().focused;
    if let Some(h) = handler {
      // SAFETY: the runtime's handler.
      unsafe { (h.f)(h.data as *mut c_void, window_id, c_int::from(focused)) };
    }
  });
}

pub fn emit_resize(window_id: u32, width: i32, height: i32) {
  ui::post_call(move || {
    let handler = handlers().resize;
    if let Some(h) = handler {
      // SAFETY: the runtime's handler.
      unsafe { (h.f)(h.data as *mut c_void, window_id, width, height) };
    }
  });
}

pub fn emit_moved(window_id: u32, x: i32, y: i32) {
  ui::post_call(move || {
    let handler = handlers().moved;
    if let Some(h) = handler {
      // SAFETY: the runtime's handler.
      unsafe { (h.f)(h.data as *mut c_void, window_id, x, y) };
    }
  });
}

// --- Windows ----------------------------------------------------------------------------

static NEXT_WINDOW_ID: AtomicU32 = AtomicU32::new(1);
/// The ids of the windows that exist or are about to (their creation is queued).
static LIVE_WINDOWS: Mutex<Option<HashSet<u32>>> = Mutex::new(None);
/// `quit` or `exit_app` was called: no new windows.
static QUITTING: AtomicBool = AtomicBool::new(false);
static QUIT_ON_LAST_WINDOW_CLOSED: AtomicBool = AtomicBool::new(true);
static EXIT_CODE: Mutex<Option<i32>> = Mutex::new(None);

fn live_windows() -> std::sync::MutexGuard<'static, Option<HashSet<u32>>> {
  LIVE_WINDOWS.lock().unwrap_or_else(|e| e.into_inner())
}

/// Whether `window_id` names a window (any thread).
pub fn window_exists(window_id: u32) -> bool {
  live_windows()
    .as_ref()
    .is_some_and(|ids| ids.contains(&window_id))
}

thread_local! {
  /// The windows, on the UI thread.
  static WINDOWS: RefCell<HashMap<u32, WindowHandle<TreeView>>> =
    RefCell::new(HashMap::new());
  static BY_GPUI_ID: RefCell<HashMap<WindowId, u32>> = RefCell::new(HashMap::new());
}

/// A new window id, with its creation queued on the UI thread: every later call for the id
/// queues behind it. 0 once the app is quitting.
pub fn create_window(flags: u32) -> u32 {
  if QUITTING.load(Ordering::SeqCst) {
    return 0;
  }
  let id = NEXT_WINDOW_ID.fetch_add(1, Ordering::Relaxed);
  live_windows().get_or_insert_with(HashSet::new).insert(id);
  ui::post(move |app| open_window(app, id, flags));
  id
}

fn open_window(app: &mut App, id: u32, flags: u32) {
  let hidden = flags & FLAG_HIDDEN != 0;
  let bounds =
    Bounds::centered(None, size(px(DEFAULT_SIZE.0), px(DEFAULT_SIZE.1)), app);
  let options = WindowOptions {
    window_bounds: Some(WindowBounds::Windowed(bounds)),
    titlebar: (flags & FLAG_FRAMELESS == 0).then(TitlebarOptions::default),
    show: !hidden,
    focus: !hidden && flags & FLAG_NO_ACTIVATE == 0,
    ..Default::default()
  };
  let opened = app.open_window(options, move |window, cx| {
    cx.new(|cx| TreeView::new(id, !hidden, window, cx))
  });
  match opened {
    Ok(handle) => {
      BY_GPUI_ID.with(|map| map.borrow_mut().insert(handle.window_id(), id));
      WINDOWS.with(|map| map.borrow_mut().insert(id, handle));
      if !hidden && flags & FLAG_NO_ACTIVATE == 0 {
        app.activate(true);
      }
    }
    Err(error) => {
      eprintln!("laufey_gpui: window {id} could not be opened: {error:#}");
      forget_window(id);
    }
  }
}

fn forget_window(id: u32) {
  if let Some(ids) = live_windows().as_mut() {
    ids.remove(&id);
  }
  WINDOWS.with(|map| map.borrow_mut().remove(&id));
  BY_GPUI_ID.with(|map| map.borrow_mut().retain(|_, v| *v != id));
}

/// Run `f` on window `id`'s view, on the UI thread. `None` for an unknown window.
pub fn with_window<R>(
  app: &mut App,
  id: u32,
  f: impl FnOnce(&mut TreeView, &mut Window, &mut Context<TreeView>) -> R,
) -> Option<R> {
  // The handle is copied out: `f` may open or close windows.
  let handle = WINDOWS.with(|map| map.borrow().get(&id).copied())?;
  handle.update(app, f).ok()
}

/// Run `f` on window `id` without borrowing its view: for what runs the window's own
/// listeners (input dispatch), which may reach the view themselves.
pub fn with_gpui_window<R>(
  app: &mut App,
  id: u32,
  f: impl FnOnce(&mut Window, &mut App) -> R,
) -> Option<R> {
  let handle = WINDOWS.with(|map| map.borrow().get(&id).copied())?;
  AnyWindowHandle::from(handle)
    .update(app, |_, window, cx| f(window, cx))
    .ok()
}

/// `close_window`: really close it, whatever a close-requested handler said.
pub fn close_window(app: &mut App, id: u32) {
  let closed = with_window(app, id, |_, window, _| window.remove_window());
  if closed.is_some() {
    window_gone(app, id);
  }
}

/// GPUI closed a window (the user did, with no close-requested handler registered).
pub fn gpui_window_closed(app: &mut App, gpui_id: WindowId) {
  let id = BY_GPUI_ID.with(|map| map.borrow().get(&gpui_id).copied());
  if let Some(id) = id {
    window_gone(app, id);
  }
}

fn window_gone(app: &mut App, id: u32) {
  let known = WINDOWS.with(|map| map.borrow().contains_key(&id));
  if !known {
    return;
  }
  forget_window(id);
  let none_left = live_windows().as_ref().is_none_or(HashSet::is_empty);
  if none_left && QUIT_ON_LAST_WINDOW_CLOSED.load(Ordering::SeqCst) {
    quit(app, None);
  }
}

pub fn set_quit_on_last_window_closed(quit: bool) {
  QUIT_ON_LAST_WINDOW_CLOSED.store(quit, Ordering::SeqCst);
}

/// End the app; `code` is `exit_app`'s exit code (the first one given wins). The host stops
/// the runtime and ends the process itself (`finish`), without asking the platform to quit.
pub fn quit(_app: &mut App, code: Option<i32>) -> ! {
  note_quitting(code);
  crate::finish()
}

/// Record that the app is quitting (any thread): windows are refused from now on.
pub fn note_quitting(code: Option<i32>) {
  QUITTING.store(true, Ordering::SeqCst);
  if let Some(code) = code {
    EXIT_CODE
      .lock()
      .unwrap_or_else(|e| e.into_inner())
      .get_or_insert(code);
  }
}

/// `exit_app`'s code, when it was called.
pub fn requested_exit_code() -> Option<i32> {
  *EXIT_CODE.lock().unwrap_or_else(|e| e.into_inner())
}
