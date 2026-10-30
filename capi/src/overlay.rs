// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Overlay windows, tray events, the activation policy and text to speech
//! (API 50). See `docs/window-management.md` ("Overlay windows"),
//! `docs/tray.md` and `docs/dock-taskbar.md`.

use std::collections::HashMap;
use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::{Arc, Mutex, OnceLock};

use crate::io::take_backend_string;
use crate::{api, ffi, try_api, LaufeyBackendApi, Window};

pub const LAUFEY_WINDOW_LEVEL_NORMAL: i32 = 0;
pub const LAUFEY_WINDOW_LEVEL_FLOATING: i32 = 1;
pub const LAUFEY_WINDOW_LEVEL_STATUS: i32 = 2;
pub const LAUFEY_WINDOW_LEVEL_POP_UP_MENU: i32 = 3;
pub const LAUFEY_WINDOW_LEVEL_SCREEN_SAVER: i32 = 4;

pub const LAUFEY_OVERLAY_CAP_CLICK_PASSTHROUGH: u32 = 1 << 0;
pub const LAUFEY_OVERLAY_CAP_PASSTHROUGH_FORWARD: u32 = 1 << 1;
pub const LAUFEY_OVERLAY_CAP_INPUT_REGION: u32 = 1 << 2;
pub const LAUFEY_OVERLAY_CAP_SKIP_TASKBAR: u32 = 1 << 3;
pub const LAUFEY_OVERLAY_CAP_ALL_WORKSPACES: u32 = 1 << 4;
pub const LAUFEY_OVERLAY_CAP_VISIBLE_ON_FULL_SCREEN: u32 = 1 << 5;
pub const LAUFEY_OVERLAY_CAP_WINDOW_LEVELS: u32 = 1 << 6;
pub const LAUFEY_OVERLAY_CAP_TRANSPARENT: u32 = 1 << 7;
pub const LAUFEY_OVERLAY_CAP_LAYER_SHELL: u32 = 1 << 8;
pub const LAUFEY_OVERLAY_CAP_ACTIVATION_POLICY: u32 = 1 << 9;
pub const LAUFEY_OVERLAY_CAP_NON_ACTIVATING: u32 = 1 << 10;

pub const LAUFEY_ACTIVATION_POLICY_REGULAR: i32 = 0;
pub const LAUFEY_ACTIVATION_POLICY_ACCESSORY: i32 = 1;
pub const LAUFEY_ACTIVATION_POLICY_PROHIBITED: i32 = 2;

pub const LAUFEY_TRAY_EVENT_CLICK: i32 = 1;
pub const LAUFEY_TRAY_EVENT_DOUBLE_CLICK: i32 = 2;
pub const LAUFEY_TRAY_EVENT_RIGHT_CLICK: i32 = 3;
pub const LAUFEY_TRAY_EVENT_MIDDLE_CLICK: i32 = 4;
pub const LAUFEY_TRAY_EVENT_MOUSE_ENTER: i32 = 5;
pub const LAUFEY_TRAY_EVENT_MOUSE_LEAVE: i32 = 6;

pub const LAUFEY_TRAY_CAP_CLICK: u32 = 1 << 0;
pub const LAUFEY_TRAY_CAP_DOUBLE_CLICK: u32 = 1 << 1;
pub const LAUFEY_TRAY_CAP_RIGHT_CLICK: u32 = 1 << 2;
pub const LAUFEY_TRAY_CAP_MIDDLE_CLICK: u32 = 1 << 3;
pub const LAUFEY_TRAY_CAP_HOVER: u32 = 1 << 4;
pub const LAUFEY_TRAY_CAP_MODIFIERS: u32 = 1 << 5;
pub const LAUFEY_TRAY_CAP_POSITION: u32 = 1 << 6;
pub const LAUFEY_TRAY_CAP_POP_UP_MENU: u32 = 1 << 7;

pub const LAUFEY_SPEECH_DONE: i32 = 0;
pub const LAUFEY_SPEECH_CANCELLED: i32 = 1;
pub const LAUFEY_SPEECH_FAILED: i32 = 2;
pub const LAUFEY_SPEECH_UNSUPPORTED: i32 = 3;

/// What the overlay-window calls do on this backend and OS (API 50). All
/// `false` on a backend older than API 50.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct OverlayCapabilities {
  bits: u32,
}

impl OverlayCapabilities {
  pub fn from_bits(bits: u32) -> Self {
    OverlayCapabilities { bits }
  }
  pub fn bits(&self) -> u32 {
    self.bits
  }
  fn has(&self, bit: u32) -> bool {
    self.bits & bit != 0
  }
  /// [`Window::set_click_passthrough`] works.
  pub fn click_passthrough(&self) -> bool {
    self.has(LAUFEY_OVERLAY_CAP_CLICK_PASSTHROUGH)
  }
  /// [`Window::set_click_passthrough_forward`] reports moves.
  pub fn passthrough_forward(&self) -> bool {
    self.has(LAUFEY_OVERLAY_CAP_PASSTHROUGH_FORWARD)
  }
  /// [`Window::set_input_region`] works.
  pub fn input_region(&self) -> bool {
    self.has(LAUFEY_OVERLAY_CAP_INPUT_REGION)
  }
  /// [`Window::set_skip_taskbar`] works.
  pub fn skip_taskbar(&self) -> bool {
    self.has(LAUFEY_OVERLAY_CAP_SKIP_TASKBAR)
  }
  /// [`Window::set_visible_on_all_workspaces`] works.
  pub fn all_workspaces(&self) -> bool {
    self.has(LAUFEY_OVERLAY_CAP_ALL_WORKSPACES)
  }
  /// ... and its `visible_on_full_screen` too (macOS).
  pub fn visible_on_full_screen(&self) -> bool {
    self.has(LAUFEY_OVERLAY_CAP_VISIBLE_ON_FULL_SCREEN)
  }
  /// [`Window::set_level`] works.
  pub fn window_levels(&self) -> bool {
    self.has(LAUFEY_OVERLAY_CAP_WINDOW_LEVELS)
  }
  /// A `transparent` window shows what is behind it.
  pub fn transparent(&self) -> bool {
    self.has(LAUFEY_OVERLAY_CAP_TRANSPARENT)
  }
  /// An `overlay` window is a wlr-layer-shell surface.
  pub fn layer_shell(&self) -> bool {
    self.has(LAUFEY_OVERLAY_CAP_LAYER_SHELL)
  }
  /// [`set_activation_policy`] works (macOS).
  pub fn activation_policy(&self) -> bool {
    self.has(LAUFEY_OVERLAY_CAP_ACTIVATION_POLICY)
  }
  /// A `no_activate` window is a real non-activating panel.
  pub fn non_activating(&self) -> bool {
    self.has(LAUFEY_OVERLAY_CAP_NON_ACTIVATING)
  }
}

pub(crate) fn overlay_capabilities_with(
  api: &LaufeyBackendApi,
) -> OverlayCapabilities {
  match api.overlay_capabilities {
    Some(f) => OverlayCapabilities::from_bits(unsafe { f(api.backend_data) }),
    None => OverlayCapabilities::default(),
  }
}

/// The overlay capabilities of this backend (API 50).
pub fn overlay_capabilities() -> OverlayCapabilities {
  try_api().map(overlay_capabilities_with).unwrap_or_default()
}

/// A rectangle in a window's content area, in DIPs from its top-left.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct InputRect {
  pub x: i32,
  pub y: i32,
  pub width: i32,
  pub height: i32,
}

pub(crate) fn set_input_region_with(
  api: &LaufeyBackendApi,
  window_id: u32,
  rects: Option<&[InputRect]>,
) {
  let Some(f) = api.set_input_region else {
    return;
  };
  match rects {
    None => unsafe { f(api.backend_data, window_id, std::ptr::null(), 0) },
    Some(rects) => {
      let raw: Vec<ffi::laufey_rect_t> = rects
        .iter()
        .filter(|r| r.width > 0 && r.height > 0)
        .map(|r| ffi::laufey_rect_t {
          x: r.x,
          y: r.y,
          width: r.width,
          height: r.height,
        })
        .collect();
      // A non-NULL pointer even for an empty list: "nothing takes input".
      let empty = ffi::laufey_rect_t {
        x: 0,
        y: 0,
        width: 0,
        height: 0,
      };
      let ptr = if raw.is_empty() {
        &empty as *const _
      } else {
        raw.as_ptr()
      };
      unsafe { f(api.backend_data, window_id, ptr, raw.len()) };
    }
  }
}

impl Window {
  /// Only `rects` take mouse input; elsewhere clicks reach what is beneath
  /// the window. `None` removes the region (API 50).
  pub fn set_input_region(&self, rects: Option<&[InputRect]>) {
    set_input_region_with(api(), self.id(), rects);
  }

  /// Keep the window off the taskbar / task list (API 50). `false` where
  /// it has no effect.
  pub fn set_skip_taskbar(&self, skip: bool) -> bool {
    let api = api();
    match api.set_skip_taskbar {
      Some(f) => unsafe { f(api.backend_data, self.id(), skip) },
      None => false,
    }
  }

  /// Show the window on every Space / virtual desktop, and over full-screen
  /// apps with `visible_on_full_screen` (API 50).
  pub fn set_visible_on_all_workspaces(
    &self,
    visible: bool,
    visible_on_full_screen: bool,
  ) -> bool {
    let api = api();
    match api.set_visible_on_all_workspaces {
      Some(f) => unsafe {
        f(api.backend_data, self.id(), visible, visible_on_full_screen)
      },
      None => false,
    }
  }

  /// Set the window level (`LAUFEY_WINDOW_LEVEL_*`, API 50).
  pub fn set_level(&self, level: i32) -> bool {
    let api = api();
    match api.set_window_level {
      Some(f) => unsafe { f(api.backend_data, self.id(), level) },
      None => false,
    }
  }
}

/// Set the app's activation policy (`LAUFEY_ACTIVATION_POLICY_*`, macOS;
/// API 50). `false` where there is none.
pub fn set_activation_policy(policy: i32) -> bool {
  let api = api();
  match api.set_activation_policy {
    Some(f) => unsafe { f(api.backend_data, policy) },
    None => false,
  }
}

/// The app's activation policy, or `None` where there is none.
pub fn activation_policy() -> Option<i32> {
  let api = try_api()?;
  let f = api.get_activation_policy?;
  let p = unsafe { f(api.backend_data) };
  (p >= 0).then_some(p)
}

/// The mouse button of a tray event.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrayButton {
  Left,
  Right,
  Middle,
}

/// What happened on a tray icon.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TrayEventKind {
  Click,
  DoubleClick,
  RightClick,
  MiddleClick,
  MouseEnter,
  MouseLeave,
}

/// One tray event (API 50).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TrayEvent {
  pub tray_id: u32,
  pub kind: TrayEventKind,
  pub button: TrayButton,
  /// `LAUFEY_MOD_*` bits.
  pub modifiers: u32,
  /// The cursor, in window-position screen space, where the OS says.
  pub position: Option<(i32, i32)>,
}

impl TrayEvent {
  pub(crate) fn from_raw(e: &ffi::laufey_tray_event_t) -> Option<Self> {
    let kind = match e.kind {
      LAUFEY_TRAY_EVENT_CLICK => TrayEventKind::Click,
      LAUFEY_TRAY_EVENT_DOUBLE_CLICK => TrayEventKind::DoubleClick,
      LAUFEY_TRAY_EVENT_RIGHT_CLICK => TrayEventKind::RightClick,
      LAUFEY_TRAY_EVENT_MIDDLE_CLICK => TrayEventKind::MiddleClick,
      LAUFEY_TRAY_EVENT_MOUSE_ENTER => TrayEventKind::MouseEnter,
      LAUFEY_TRAY_EVENT_MOUSE_LEAVE => TrayEventKind::MouseLeave,
      _ => return None,
    };
    let button = match e.button as u32 {
      ffi::LAUFEY_MOUSE_BUTTON_RIGHT => TrayButton::Right,
      ffi::LAUFEY_MOUSE_BUTTON_MIDDLE => TrayButton::Middle,
      _ => TrayButton::Left,
    };
    Some(TrayEvent {
      tray_id: e.tray_id,
      kind,
      button,
      modifiers: e.modifiers,
      position: e.has_position.then_some((e.x, e.y)),
    })
  }
}

type TrayEventHandler = Arc<dyn Fn(TrayEvent) + Send + Sync>;

fn tray_event_handlers() -> &'static Mutex<HashMap<u32, TrayEventHandler>> {
  static H: OnceLock<Mutex<HashMap<u32, TrayEventHandler>>> = OnceLock::new();
  H.get_or_init(|| Mutex::new(HashMap::new()))
}

unsafe extern "C" fn tray_event_callback(
  _: *mut c_void,
  event: *const ffi::laufey_tray_event_t,
) {
  if event.is_null() {
    return;
  }
  let Some(e) = TrayEvent::from_raw(unsafe { &*event }) else {
    return;
  };
  let handler = tray_event_handlers()
    .lock()
    .unwrap()
    .get(&e.tray_id)
    .cloned();
  if let Some(h) = handler {
    h(e);
  }
}

/// Every event of tray `tray_id` with its detail (API 50); `None` clears
/// it. Returns `false` where the backend has no tray events.
pub fn set_tray_event_handler(
  tray_id: u32,
  handler: Option<Box<dyn Fn(TrayEvent) + Send + Sync>>,
) -> bool {
  let api = api();
  let Some(f) = api.set_tray_event_handler else {
    return false;
  };
  match handler {
    Some(h) => {
      tray_event_handlers()
        .lock()
        .unwrap()
        .insert(tray_id, Arc::from(h));
      unsafe {
        f(
          api.backend_data,
          tray_id,
          Some(tray_event_callback),
          std::ptr::null_mut(),
        )
      };
    }
    None => {
      tray_event_handlers().lock().unwrap().remove(&tray_id);
      unsafe { f(api.backend_data, tray_id, None, std::ptr::null_mut()) };
    }
  }
  true
}

/// Which tray events and data this backend delivers (`LAUFEY_TRAY_CAP_*`).
pub fn tray_capabilities() -> u32 {
  try_api()
    .and_then(|api| api.tray_capabilities.map(|f| (api, f)))
    .map(|(api, f)| unsafe { f(api.backend_data) })
    .unwrap_or(0)
}

/// Open the tray's menu at the icon now. `false` without a menu or where
/// the OS can't.
pub fn pop_up_tray_menu(tray_id: u32) -> bool {
  let api = api();
  match api.pop_up_tray_menu {
    Some(f) => unsafe { f(api.backend_data, tray_id) },
    None => false,
  }
}

/// A text-to-speech request (API 50).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct SpeechRequest {
  pub text: String,
  pub voice: Option<String>,
  pub lang: Option<String>,
  /// 0.1..10, 1 is normal.
  pub rate: Option<f64>,
  /// 0..2, 1 is normal.
  pub pitch: Option<f64>,
  /// 0..1.
  pub volume: Option<f64>,
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

fn json_number(n: f64) -> Option<String> {
  n.is_finite().then(|| format!("{n}"))
}

impl SpeechRequest {
  pub(crate) fn to_json(&self) -> String {
    let mut parts = vec![format!("\"text\":{}", json_string(&self.text))];
    if let Some(v) = &self.voice {
      parts.push(format!("\"voice\":{}", json_string(v)));
    }
    if let Some(l) = &self.lang {
      parts.push(format!("\"lang\":{}", json_string(l)));
    }
    for (key, value) in [
      ("rate", self.rate),
      ("pitch", self.pitch),
      ("volume", self.volume),
    ] {
      if let Some(n) = value.and_then(json_number) {
        parts.push(format!("\"{key}\":{n}"));
      }
    }
    format!("{{{}}}", parts.join(","))
  }
}

/// How an utterance ended.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SpeechOutcome {
  Done,
  Cancelled,
  Failed(String),
  Unsupported,
}

type SpeechCallback = Box<dyn FnOnce(SpeechOutcome) + Send>;

unsafe extern "C" fn speech_trampoline(
  user_data: *mut c_void,
  _utterance: u32,
  status: i32,
  message: *const c_char,
) {
  let cb = unsafe { Box::from_raw(user_data as *mut SpeechCallback) };
  let message = if message.is_null() {
    String::new()
  } else {
    unsafe { CStr::from_ptr(message) }
      .to_string_lossy()
      .into_owned()
  };
  cb(match status {
    LAUFEY_SPEECH_DONE => SpeechOutcome::Done,
    LAUFEY_SPEECH_CANCELLED => SpeechOutcome::Cancelled,
    LAUFEY_SPEECH_UNSUPPORTED => SpeechOutcome::Unsupported,
    _ => SpeechOutcome::Failed(message),
  });
}

pub(crate) fn speak_with<F>(
  api: &LaufeyBackendApi,
  request: &SpeechRequest,
  callback: F,
) -> u32
where
  F: FnOnce(SpeechOutcome) + Send + 'static,
{
  let Some(f) = api.speak else {
    callback(SpeechOutcome::Unsupported);
    return 0;
  };
  let Ok(json) = CString::new(request.to_json()) else {
    return 0;
  };
  let boxed: Box<SpeechCallback> = Box::new(Box::new(callback));
  let raw = Box::into_raw(boxed);
  let id = unsafe {
    f(
      api.backend_data,
      json.as_ptr(),
      Some(speech_trampoline),
      raw as *mut c_void,
    )
  };
  if id == 0 {
    // Refused: the backend keeps no callback.
    drop(unsafe { Box::from_raw(raw) });
  }
  id
}

/// Speak through the OS engine (API 50). Returns the utterance id, or 0 for
/// an invalid request (the callback is dropped then), or when the backend
/// has no speech entry point (the callback has reported `Unsupported`).
pub fn speak<F>(request: &SpeechRequest, callback: F) -> u32
where
  F: FnOnce(SpeechOutcome) + Send + 'static,
{
  speak_with(api(), request, callback)
}

/// Stop speaking.
pub fn stop_speaking() {
  let api = api();
  if let Some(f) = api.stop_speaking {
    unsafe { f(api.backend_data) };
  }
}

/// The installed voices as a JSON array of `{"id", "name", "lang",
/// "default"}`; `"[]"` where there are none.
pub fn speech_voices_json() -> String {
  let api = api();
  let Some(f) = api.speech_voices else {
    return "[]".into();
  };
  unsafe { take_backend_string(api, f(api.backend_data)) }
    .unwrap_or_else(|| "[]".into())
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::sync::atomic::{AtomicUsize, Ordering};

  static RECTS: Mutex<Option<Vec<(i32, i32, i32, i32)>>> = Mutex::new(None);
  static NULL_RECTS: AtomicUsize = AtomicUsize::new(0);

  unsafe extern "C" fn fake_set_input_region(
    _: *mut c_void,
    _window: u32,
    rects: *const ffi::laufey_rect_t,
    count: usize,
  ) {
    if rects.is_null() {
      NULL_RECTS.fetch_add(1, Ordering::SeqCst);
      return;
    }
    let v = unsafe { std::slice::from_raw_parts(rects, count) }
      .iter()
      .map(|r| (r.x, r.y, r.width, r.height))
      .collect();
    *RECTS.lock().unwrap() = Some(v);
  }

  #[test]
  fn input_region_passes_rects_drops_empty_ones_and_clears_with_null() {
    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    api.set_input_region = Some(fake_set_input_region);
    let rects = [
      InputRect {
        x: 1,
        y: 2,
        width: 3,
        height: 4,
      },
      InputRect {
        x: 9,
        y: 9,
        width: 0,
        height: 5,
      },
    ];
    set_input_region_with(&api, 1, Some(&rects));
    assert_eq!(*RECTS.lock().unwrap(), Some(vec![(1, 2, 3, 4)]));
    // Empty list: a non-NULL pointer with no rectangles (nothing takes
    // input), not "no region".
    set_input_region_with(&api, 1, Some(&[]));
    assert_eq!(*RECTS.lock().unwrap(), Some(vec![]));
    let before = NULL_RECTS.load(Ordering::SeqCst);
    set_input_region_with(&api, 1, None);
    assert_eq!(NULL_RECTS.load(Ordering::SeqCst), before + 1);
  }

  #[test]
  fn capabilities_are_all_false_without_the_entry_point() {
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    let caps = overlay_capabilities_with(&none);
    assert!(!caps.input_region() && !caps.layer_shell());
    let caps = OverlayCapabilities::from_bits(
      LAUFEY_OVERLAY_CAP_INPUT_REGION | LAUFEY_OVERLAY_CAP_LAYER_SHELL,
    );
    assert!(caps.input_region() && caps.layer_shell());
    assert!(!caps.skip_taskbar());
  }

  #[test]
  fn tray_events_decode() {
    let raw = ffi::laufey_tray_event_t {
      tray_id: 3,
      kind: LAUFEY_TRAY_EVENT_RIGHT_CLICK,
      button: ffi::LAUFEY_MOUSE_BUTTON_RIGHT as i32,
      modifiers: ffi::LAUFEY_MOD_ALT as u32,
      x: 10,
      y: 20,
      has_position: true,
    };
    let e = TrayEvent::from_raw(&raw).unwrap();
    assert_eq!(e.kind, TrayEventKind::RightClick);
    assert_eq!(e.button, TrayButton::Right);
    assert_eq!(e.position, Some((10, 20)));
    let unknown = ffi::laufey_tray_event_t { kind: 99, ..raw };
    assert!(TrayEvent::from_raw(&unknown).is_none());
    let no_pos = ffi::laufey_tray_event_t {
      has_position: false,
      ..raw
    };
    assert_eq!(TrayEvent::from_raw(&no_pos).unwrap().position, None);
  }

  #[test]
  fn speech_request_json() {
    let r = SpeechRequest {
      text: "Hi \"there\"\n".into(),
      voice: Some("v1".into()),
      lang: None,
      rate: Some(1.5),
      pitch: Some(f64::NAN),
      volume: Some(0.5),
    };
    assert_eq!(
      r.to_json(),
      r#"{"text":"Hi \"there\"\u000a","voice":"v1","rate":1.5,"volume":0.5}"#
    );
  }

  unsafe extern "C" fn fake_speak(
    _: *mut c_void,
    _json: *const c_char,
    cb: ffi::laufey_speech_fn,
    ud: *mut c_void,
  ) -> u32 {
    let msg = CString::new("").unwrap();
    unsafe { cb.unwrap()(ud, 5, LAUFEY_SPEECH_DONE, msg.as_ptr()) };
    5
  }

  unsafe extern "C" fn fake_speak_refuses(
    _: *mut c_void,
    _json: *const c_char,
    _cb: ffi::laufey_speech_fn,
    _ud: *mut c_void,
  ) -> u32 {
    0
  }

  #[test]
  fn speak_reports_once_and_frees_a_refused_callback() {
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    let (tx, rx) = std::sync::mpsc::channel();
    let tx2 = tx.clone();
    assert_eq!(
      speak_with(&none, &SpeechRequest::default(), move |o| {
        let _ = tx2.send(o);
      }),
      0
    );
    assert_eq!(rx.recv().unwrap(), SpeechOutcome::Unsupported);

    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    api.speak = Some(fake_speak);
    let tx3 = tx.clone();
    let id = speak_with(&api, &SpeechRequest::default(), move |o| {
      let _ = tx3.send(o);
    });
    assert_eq!(id, 5);
    assert_eq!(rx.recv().unwrap(), SpeechOutcome::Done);

    api.speak = Some(fake_speak_refuses);
    let guard = Arc::new(());
    let held = guard.clone();
    assert_eq!(
      speak_with(&api, &SpeechRequest::default(), move |_| drop(held)),
      0
    );
    // The refused callback was dropped, with what it held.
    assert_eq!(Arc::strong_count(&guard), 1);
  }
}
