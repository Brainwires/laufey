//! The API table this host hands the runtime (`laufey_backend_api_t`, docs/c-abi.md).
//!
//! What a native-tree app needs is implemented: windows (create, close, title, size, show,
//! focus, the getters), quitting, the window event handlers, UI-thread tasks, capabilities,
//! the input test hook, and the native tree. Every other entry stays NULL, which is how a
//! laufey backend says "not supported": the `laufey` crate null-checks each one. The per-entry
//! table is in docs/backends.md ("GPUI").
//!
//! Every entry may be called from any thread. Setters queue their work on the UI thread and
//! return; getters wait for it and answer their defaults once the loop has ended.

use std::ffi::{CStr, CString, c_char, c_int, c_void};

use gpui::{
  KeyDownEvent, KeyUpEvent, Keystroke, Modifiers, MouseButton, MouseDownEvent,
  MouseMoveEvent, MouseUpEvent, PlatformInput, ScrollDelta, ScrollEvent,
  ScrollSource, TouchPhase, point, px, size,
};
use laufey::LaufeyBackendApi;

use crate::host::{self, Handler};
use crate::ui;

/// The batch format this host takes.
const NATIVE_TREE_VERSION: u32 = laufey_ntb1::VERSION as u32;

/// `LAUFEY_TEST_INPUT_*` (laufey.h).
const TEST_INPUT_KEY: c_int = 0;
const TEST_INPUT_MOUSE_MOVE: c_int = 1;
const TEST_INPUT_MOUSE_BUTTON: c_int = 2;
const TEST_INPUT_WHEEL: c_int = 3;

/// `LAUFEY_MOD_*` (laufey.h).
pub const MOD_SHIFT: u32 = 1 << 0;
pub const MOD_CONTROL: u32 = 1 << 1;
pub const MOD_ALT: u32 = 1 << 2;
pub const MOD_META: u32 = 1 << 3;

/// GPUI's modifiers as `LAUFEY_MOD_*` bits.
pub fn modifiers_to_laufey(m: &Modifiers) -> u32 {
  let mut bits = 0;
  if m.shift {
    bits |= MOD_SHIFT;
  }
  if m.control {
    bits |= MOD_CONTROL;
  }
  if m.alt {
    bits |= MOD_ALT;
  }
  if m.platform {
    bits |= MOD_META;
  }
  bits
}

fn modifiers_from_laufey(bits: u32) -> Modifiers {
  Modifiers {
    shift: bits & MOD_SHIFT != 0,
    control: bits & MOD_CONTROL != 0,
    alt: bits & MOD_ALT != 0,
    platform: bits & MOD_META != 0,
    ..Default::default()
  }
}

/// `LAUFEY_MOUSE_BUTTON_*` for a GPUI button.
pub fn button_to_laufey(button: MouseButton) -> c_int {
  match button {
    MouseButton::Left => 0,
    MouseButton::Right => 1,
    MouseButton::Middle => 2,
    MouseButton::Navigate(gpui::NavigationDirection::Back) => 3,
    MouseButton::Navigate(gpui::NavigationDirection::Forward) => 4,
  }
}

fn button_from_laufey(button: c_int) -> Option<MouseButton> {
  Some(match button {
    0 => MouseButton::Left,
    1 => MouseButton::Right,
    2 => MouseButton::Middle,
    3 => MouseButton::Navigate(gpui::NavigationDirection::Back),
    4 => MouseButton::Navigate(gpui::NavigationDirection::Forward),
    _ => return None,
  })
}

// --- Windows ----------------------------------------------------------------------------

unsafe extern "C" fn create_window(_: *mut c_void) -> u32 {
  host::create_window(0)
}

unsafe extern "C" fn create_window_ex(_: *mut c_void, flags: u32) -> u32 {
  host::create_window(flags)
}

unsafe extern "C" fn close_window(_: *mut c_void, window_id: u32) {
  ui::post(move |app| host::close_window(app, window_id));
}

unsafe extern "C" fn set_title(
  _: *mut c_void,
  window_id: u32,
  title: *const c_char,
) {
  if title.is_null() {
    return;
  }
  // SAFETY: a NUL-terminated string, valid for the call (laufey.h).
  let title = unsafe { CStr::from_ptr(title) }
    .to_string_lossy()
    .into_owned();
  ui::post(move |app| {
    host::with_window(app, window_id, |_, window, _| {
      window.set_window_title(&title)
    });
  });
}

unsafe extern "C" fn set_window_size(
  _: *mut c_void,
  window_id: u32,
  width: c_int,
  height: c_int,
) {
  if width <= 0 || height <= 0 {
    return;
  }
  ui::post(move |app| {
    host::with_window(app, window_id, |_, window, _| {
      window.resize(size(px(width as f32), px(height as f32)))
    });
  });
}

unsafe extern "C" fn get_window_size(
  _: *mut c_void,
  window_id: u32,
  width: *mut c_int,
  height: *mut c_int,
) {
  let answer = ui::run_sync(move |app| {
    host::with_window(app, window_id, |_, window, _| {
      let size = window.viewport_size();
      (
        f32::from(size.width).round() as c_int,
        f32::from(size.height).round() as c_int,
      )
    })
  })
  .flatten()
  .unwrap_or((0, 0));
  // SAFETY: the caller's out pointers, null or writable (laufey.h).
  unsafe {
    if !width.is_null() {
      *width = answer.0;
    }
    if !height.is_null() {
      *height = answer.1;
    }
  }
}

unsafe extern "C" fn get_window_outer_size(
  _: *mut c_void,
  window_id: u32,
  width: *mut c_int,
  height: *mut c_int,
) {
  let answer = ui::run_sync(move |app| {
    host::with_window(app, window_id, |_, window, _| {
      let size = window.bounds().size;
      (
        f32::from(size.width).round() as c_int,
        f32::from(size.height).round() as c_int,
      )
    })
  })
  .flatten()
  .unwrap_or((0, 0));
  // SAFETY: as in get_window_size.
  unsafe {
    if !width.is_null() {
      *width = answer.0;
    }
    if !height.is_null() {
      *height = answer.1;
    }
  }
}

unsafe extern "C" fn get_window_position(
  _: *mut c_void,
  window_id: u32,
  x: *mut c_int,
  y: *mut c_int,
) {
  let answer = ui::run_sync(move |app| {
    host::with_window(app, window_id, |_, window, _| {
      let origin = window.bounds().origin;
      (
        f32::from(origin.x).round() as c_int,
        f32::from(origin.y).round() as c_int,
      )
    })
  })
  .flatten()
  .unwrap_or((0, 0));
  // SAFETY: as in get_window_size.
  unsafe {
    if !x.is_null() {
      *x = answer.0;
    }
    if !y.is_null() {
      *y = answer.1;
    }
  }
}

unsafe extern "C" fn get_window_scale_factor(
  _: *mut c_void,
  window_id: u32,
) -> f64 {
  ui::run_sync(move |app| {
    host::with_window(app, window_id, |_, window, _| {
      f64::from(window.scale_factor())
    })
  })
  .flatten()
  .unwrap_or(0.0)
}

unsafe extern "C" fn is_resizable(_: *mut c_void, window_id: u32) -> bool {
  ui::run_sync(move |app| {
    host::with_window(app, window_id, |_, window, _| window.is_resizable())
  })
  .flatten()
  .unwrap_or(false)
}

unsafe extern "C" fn is_visible(_: *mut c_void, window_id: u32) -> bool {
  ui::run_sync(move |app| {
    host::with_window(app, window_id, |view, _, _| view.is_shown())
  })
  .flatten()
  .unwrap_or(false)
}

unsafe extern "C" fn show(_: *mut c_void, window_id: u32) {
  ui::post(move |app| {
    host::with_window(app, window_id, |view, window, _| {
      view.set_shown();
      // GPUI's only way to put a window created hidden on screen.
      window.activate_window();
    });
  });
}

unsafe extern "C" fn focus(_: *mut c_void, window_id: u32) {
  ui::post(move |app| {
    let found = host::with_window(app, window_id, |view, window, _| {
      view.set_shown();
      window.activate_window();
    });
    if found.is_some() {
      app.activate(true);
    }
  });
}

unsafe extern "C" fn quit(_: *mut c_void) {
  host::note_quitting(None);
  ui::post(|app| host::quit(app, None));
}

unsafe extern "C" fn exit_app(_: *mut c_void, exit_code: c_int) {
  host::note_quitting(Some(exit_code));
  ui::post(move |app| host::quit(app, Some(exit_code)));
}

unsafe extern "C" fn set_quit_on_last_window_closed(
  _: *mut c_void,
  quit: bool,
) {
  host::set_quit_on_last_window_closed(quit);
}

unsafe extern "C" fn window_capabilities(_: *mut c_void) -> u32 {
  laufey::LAUFEY_WINDOW_CAP_NATIVE_TREE
}

unsafe extern "C" fn string_free(_: *mut c_void, s: *mut c_char) {
  if !s.is_null() {
    // SAFETY: a string this host returned (`CString::into_raw`).
    drop(unsafe { CString::from_raw(s) });
  }
}

// --- UI-thread tasks --------------------------------------------------------------------

unsafe extern "C" fn post_ui_task(
  _: *mut c_void,
  task: Option<unsafe extern "C" fn(*mut c_void)>,
  data: *mut c_void,
) {
  let Some(task) = task else {
    return;
  };
  let data = data as usize;
  // SAFETY: the caller's task and data (laufey.h); run on the UI thread.
  ui::post_call(move || unsafe { task(data as *mut c_void) });
}

unsafe extern "C" fn dispatch_ui_task(
  _: *mut c_void,
  task: Option<unsafe extern "C" fn(*mut c_void, bool)>,
  data: *mut c_void,
) {
  if let Some(task) = task {
    ui::tasks::dispatch(task, data as usize);
  }
}

unsafe extern "C" fn is_ui_thread(_: *mut c_void) -> bool {
  ui::is_ui_thread()
}

// --- Event handlers ---------------------------------------------------------------------

macro_rules! handler_setter {
  ($name:ident, $slot:ident, $ty:ty, $refresh:expr) => {
    unsafe extern "C" fn $name(
      _: *mut c_void,
      handler: Option<$ty>,
      user_data: *mut c_void,
    ) {
      host::handlers().$slot = handler.map(|f| Handler {
        f,
        data: user_data as usize,
      });
      if $refresh {
        host::refresh_windows();
      }
    }
  };
}

handler_setter!(set_keyboard_event_handler, keyboard, host::KeyboardFn, true);
handler_setter!(
  set_mouse_click_handler,
  mouse_click,
  host::MouseClickFn,
  true
);
handler_setter!(set_mouse_move_handler, mouse_move, host::MouseMoveFn, true);
handler_setter!(set_wheel_handler, wheel, host::WheelFn, true);
handler_setter!(set_focused_handler, focused, host::FocusedFn, false);
handler_setter!(set_resize_handler, resize, host::SizeFn, false);
handler_setter!(set_move_handler, moved, host::SizeFn, false);
handler_setter!(
  set_close_requested_handler,
  close_requested,
  host::WindowFn,
  false
);
handler_setter!(
  set_native_tree_event_handler,
  tree_event,
  host::TreeEventFn,
  false
);

// --- Test hook --------------------------------------------------------------------------

/// A DOM `KeyboardEvent.key` (what laufey's test input carries) as a GPUI key name.
fn gpui_key(key: &str) -> String {
  match key {
    "Enter" => "enter".into(),
    "Escape" => "escape".into(),
    "Backspace" => "backspace".into(),
    "Delete" => "delete".into(),
    "Tab" => "tab".into(),
    " " => "space".into(),
    "ArrowLeft" => "left".into(),
    "ArrowRight" => "right".into(),
    "ArrowUp" => "up".into(),
    "ArrowDown" => "down".into(),
    "Home" => "home".into(),
    "End" => "end".into(),
    "PageUp" => "pageup".into(),
    "PageDown" => "pagedown".into(),
    other => other.to_lowercase(),
  }
}

unsafe extern "C" fn test_inject_input(
  _: *mut c_void,
  window_id: u32,
  event: *const laufey::TestInputRaw,
) -> bool {
  if event.is_null() {
    return false;
  }
  // SAFETY: a valid event for the call (laufey.h); its strings are copied here.
  let event = unsafe { &*event };
  let modifiers = modifiers_from_laufey(event.modifiers);
  let position = point(px(event.x as f32), px(event.y as f32));
  let input = match event.kind {
    TEST_INPUT_KEY => {
      if event.key.is_null() {
        return false;
      }
      // SAFETY: NUL-terminated, valid for the call.
      let key = unsafe { CStr::from_ptr(event.key) }.to_string_lossy();
      let key_char =
        (key.chars().count() == 1 && !modifiers.control && !modifiers.platform)
          .then(|| key.to_string());
      let keystroke = Keystroke {
        modifiers,
        key: gpui_key(&key),
        key_char,
      };
      if event.pressed {
        PlatformInput::KeyDown(KeyDownEvent {
          keystroke,
          is_held: event.repeat,
          prefer_character_input: false,
        })
      } else {
        PlatformInput::KeyUp(KeyUpEvent { keystroke })
      }
    }
    TEST_INPUT_MOUSE_MOVE => PlatformInput::MouseMove(MouseMoveEvent {
      position,
      pressed_button: None,
      modifiers,
    }),
    TEST_INPUT_MOUSE_BUTTON => {
      let Some(button) = button_from_laufey(event.button) else {
        return false;
      };
      if event.pressed {
        PlatformInput::MouseDown(MouseDownEvent {
          button,
          position,
          modifiers,
          click_count: 1,
          first_mouse: false,
        })
      } else {
        PlatformInput::MouseUp(MouseUpEvent {
          button,
          position,
          modifiers,
          click_count: 1,
        })
      }
    }
    TEST_INPUT_WHEEL => PlatformInput::Scroll(ScrollEvent {
      position,
      // laufey's deltas are DOM-signed (positive Y scrolls down); GPUI's are the content's
      // movement.
      delta: ScrollDelta::Pixels(point(
        px(-event.delta_x as f32),
        px(-event.delta_y as f32),
      )),
      modifiers,
      touch_phase: TouchPhase::Moved,
      source: ScrollSource::Pointer,
    }),
    _ => return false,
  };
  ui::run_sync(move |app| {
    host::with_gpui_window(app, window_id, |window, cx| {
      window.dispatch_event(input, cx);
    })
    .is_some()
  })
  .unwrap_or(false)
}

// --- Native tree ------------------------------------------------------------------------

unsafe extern "C" fn native_tree_version(_: *mut c_void) -> u32 {
  NATIVE_TREE_VERSION
}

unsafe extern "C" fn native_tree_commit(
  _: *mut c_void,
  window_id: u32,
  data: *const u8,
  len: usize,
) -> i32 {
  if data.is_null() {
    return laufey::LAUFEY_NATIVE_TREE_MALFORMED;
  }
  if !host::window_exists(window_id) {
    return laufey::LAUFEY_NATIVE_TREE_NO_WINDOW;
  }
  // SAFETY: `len` bytes valid for the call (laufey.h). Decoding copies what it keeps.
  let bytes = unsafe { std::slice::from_raw_parts(data, len) };
  let batch = match laufey_ntb1::decode(bytes) {
    Ok(batch) => batch,
    Err(error) => {
      eprintln!("laufey_gpui: window {window_id}: malformed batch: {error}");
      return laufey::LAUFEY_NATIVE_TREE_MALFORMED;
    }
  };
  // One FIFO for the whole process, so a window's batches apply in commit order.
  ui::post(move |app| {
    host::with_window(app, window_id, |view, window, cx| {
      view.apply_batch(batch, window, cx)
    });
  });
  laufey::LAUFEY_NATIVE_TREE_OK
}

unsafe extern "C" fn native_tree_snapshot(
  _: *mut c_void,
  window_id: u32,
) -> *mut c_char {
  let json = ui::run_sync(move |app| {
    host::with_window(app, window_id, |view, _, cx| view.snapshot(cx))
  })
  .flatten();
  match json.and_then(|json| CString::new(json).ok()) {
    Some(json) => json.into_raw(),
    None => std::ptr::null_mut(),
  }
}

/// The table, at a static address: the runtime keeps the pointer for the life of the
/// process.
pub fn table() -> &'static LaufeyBackendApi {
  // SAFETY: the table is integers, a data pointer and `Option`s of function pointers; all
  // zero is "version 0, every entry NULL".
  let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
  api.version = laufey::LAUFEY_API_VERSION;
  api.backend_data = std::ptr::null_mut();

  api.create_window = Some(create_window);
  api.create_window_ex = Some(create_window_ex);
  api.close_window = Some(close_window);
  api.set_title = Some(set_title);
  api.set_window_size = Some(set_window_size);
  api.get_window_size = Some(get_window_size);
  api.get_window_outer_size = Some(get_window_outer_size);
  api.get_window_position = Some(get_window_position);
  api.get_window_scale_factor = Some(get_window_scale_factor);
  api.is_resizable = Some(is_resizable);
  api.is_visible = Some(is_visible);
  api.show = Some(show);
  api.focus = Some(focus);
  api.quit = Some(quit);
  api.exit_app = Some(exit_app);
  api.set_quit_on_last_window_closed = Some(set_quit_on_last_window_closed);
  api.window_capabilities = Some(window_capabilities);
  api.string_free = Some(string_free);

  api.post_ui_task = Some(post_ui_task);
  api.dispatch_ui_task = Some(dispatch_ui_task);
  api.is_ui_thread = Some(is_ui_thread);

  api.set_keyboard_event_handler = Some(set_keyboard_event_handler);
  api.set_mouse_click_handler = Some(set_mouse_click_handler);
  api.set_mouse_move_handler = Some(set_mouse_move_handler);
  api.set_wheel_handler = Some(set_wheel_handler);
  api.set_focused_handler = Some(set_focused_handler);
  api.set_resize_handler = Some(set_resize_handler);
  api.set_move_handler = Some(set_move_handler);
  api.set_close_requested_handler = Some(set_close_requested_handler);

  api.test_inject_input = Some(test_inject_input);

  api.native_tree_version = Some(native_tree_version);
  api.native_tree_commit = Some(native_tree_commit);
  api.set_native_tree_event_handler = Some(set_native_tree_event_handler);
  api.native_tree_snapshot = Some(native_tree_snapshot);

  Box::leak(Box::new(api))
}
