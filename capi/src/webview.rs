// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Child web views (API 58): a second, native browser view inside a window,
//! laid over the window's page, that loads http(s) sites as a browsing
//! context of its own (no bridge, its own storage partition, none of the
//! window's navigation rules). See `docs/webviews.md`.

use std::ffi::{c_char, c_void, CStr, CString};

use crate::io::take_backend_string;
use crate::{api, ffi, LaufeyBackendApi};

/// What this backend can do with web views ([`webview_capabilities`]).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct WebViewCapabilities(pub u32);

/// The engine that draws a backend's web views.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WebViewEngine {
  Cef,
  WkWebView,
  WebView2,
  WebKitGtk,
}

impl WebViewEngine {
  /// `"cef"`, `"wkwebview"`, `"webview2"` or `"webkitgtk"`.
  pub fn name(&self) -> &'static str {
    match self {
      WebViewEngine::Cef => "cef",
      WebViewEngine::WkWebView => "wkwebview",
      WebViewEngine::WebView2 => "webview2",
      WebViewEngine::WebKitGtk => "webkitgtk",
    }
  }
}

impl WebViewCapabilities {
  fn has(&self, bit: u32) -> bool {
    self.0 & bit != 0
  }
  /// [`create_webview`] works.
  pub fn supported(&self) -> bool {
    self.has(ffi::LAUFEY_WEBVIEW_CAP_SUPPORTED)
  }
  /// [`WebView::capture`] works.
  pub fn capture(&self) -> bool {
    self.has(ffi::LAUFEY_WEBVIEW_CAP_CAPTURE)
  }
  /// [`WebView::evaluate`] works.
  pub fn evaluate(&self) -> bool {
    self.has(ffi::LAUFEY_WEBVIEW_CAP_EVALUATE)
  }
  /// Persistent partitions keep their storage across launches (else every
  /// partition is kept in memory).
  pub fn persistent_partitions(&self) -> bool {
    self.has(ffi::LAUFEY_WEBVIEW_CAP_PERSISTENT_PARTITIONS)
  }
  /// `permissions: "ask"` prompts the user (else it acts as `"deny"`).
  pub fn permission_prompts(&self) -> bool {
    self.has(ffi::LAUFEY_WEBVIEW_CAP_PERMISSION_PROMPTS)
  }
  /// [`WebView::set_z_order`], [`WebView::raise`] and [`WebView::lower`]
  /// restack the window's views.
  pub fn z_order(&self) -> bool {
    self.has(ffi::LAUFEY_WEBVIEW_CAP_Z_ORDER)
  }
  /// [`WebView::set_zoom`] / [`WebView::zoom`].
  pub fn zoom(&self) -> bool {
    self.has(ffi::LAUFEY_WEBVIEW_CAP_ZOOM)
  }
  /// [`WebView::find`] and [`WebViewEventKind::FindResult`].
  pub fn find(&self) -> bool {
    self.has(ffi::LAUFEY_WEBVIEW_CAP_FIND)
  }
  /// [`WebView::open_devtools`].
  pub fn devtools(&self) -> bool {
    self.has(ffi::LAUFEY_WEBVIEW_CAP_DEVTOOLS)
  }
  /// [`WebViewEventKind::ContextMenu`] and `context_menu: "app"`.
  pub fn context_menu(&self) -> bool {
    self.has(ffi::LAUFEY_WEBVIEW_CAP_CONTEXT_MENU)
  }
  /// [`WebView::print`].
  pub fn print(&self) -> bool {
    self.has(ffi::LAUFEY_WEBVIEW_CAP_PRINT)
  }
  /// `clipboard_read` / `clipboard_write` are applied.
  pub fn clipboard(&self) -> bool {
    self.has(ffi::LAUFEY_WEBVIEW_CAP_CLIPBOARD)
  }
  /// The engine, `None` when unsupported.
  pub fn engine(&self) -> Option<WebViewEngine> {
    let raw = (self.0 & ffi::LAUFEY_WEBVIEW_ENGINE_MASK)
      >> ffi::LAUFEY_WEBVIEW_ENGINE_SHIFT;
    match raw {
      ffi::LAUFEY_WEBVIEW_ENGINE_CEF => Some(WebViewEngine::Cef),
      ffi::LAUFEY_WEBVIEW_ENGINE_WKWEBVIEW => Some(WebViewEngine::WkWebView),
      ffi::LAUFEY_WEBVIEW_ENGINE_WEBVIEW2 => Some(WebViewEngine::WebView2),
      ffi::LAUFEY_WEBVIEW_ENGINE_WEBKITGTK => Some(WebViewEngine::WebKitGtk),
      _ => None,
    }
  }
}

/// What this backend can do with web views; nothing on a backend without
/// the entry point (Winit). Any thread.
pub fn webview_capabilities() -> WebViewCapabilities {
  webview_capabilities_with(api())
}

fn webview_capabilities_with(api: &LaufeyBackendApi) -> WebViewCapabilities {
  match api.webview_capabilities {
    Some(f) => WebViewCapabilities(unsafe { f(api.backend_data) }),
    None => WebViewCapabilities(0),
  }
}

/// A web view's rectangle: CSS pixels relative to the top-left corner of the
/// window's page area.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct WebViewBounds {
  pub x: f64,
  pub y: f64,
  pub width: f64,
  pub height: f64,
}

/// [`create_webview`]'s options. `None` takes the backend's default (see
/// `create_webview` in laufey.h); the policy strings are passed through and
/// checked by the backend ([`WebViewError::Invalid`]).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct WebViewOptions {
  /// http(s) only.
  pub url: String,
  pub bounds: WebViewBounds,
  /// Default `"webview:default"`; `"ephemeral"` / `"ephemeral:<name>"` is
  /// kept in memory.
  pub partition: Option<String>,
  /// Default `true`.
  pub visible: Option<bool>,
  /// `"same-view"` (default), `"browser"` or `"deny"`.
  pub allow_popups: Option<String>,
  /// `"deny"` (default), `"ask"` or `"browser"`.
  pub downloads: Option<String>,
  /// `"deny"` (default) or `"ask"`.
  pub permissions: Option<String>,
  pub user_agent: Option<String>,
  /// `"native"` (default) or `"app"`.
  pub context_menu: Option<String>,
  /// Default `false`.
  pub clipboard_read: Option<bool>,
  /// Default `true` (on a user gesture).
  pub clipboard_write: Option<bool>,
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

/// A JSON number for `v` (`null` for NaN / infinity, which the backend
/// refuses as invalid).
fn json_number(v: f64) -> String {
  if v.is_finite() {
    format!("{v}")
  } else {
    "null".to_string()
  }
}

impl WebViewOptions {
  fn to_json(&self) -> String {
    let mut parts = vec![
      format!("\"url\":{}", json_string(&self.url)),
      format!("\"x\":{}", json_number(self.bounds.x)),
      format!("\"y\":{}", json_number(self.bounds.y)),
      format!("\"width\":{}", json_number(self.bounds.width)),
      format!("\"height\":{}", json_number(self.bounds.height)),
    ];
    let strings = [
      ("partition", &self.partition),
      ("allowPopups", &self.allow_popups),
      ("downloads", &self.downloads),
      ("permissions", &self.permissions),
      ("userAgent", &self.user_agent),
      ("contextMenu", &self.context_menu),
    ];
    for (key, value) in strings {
      if let Some(v) = value {
        parts.push(format!("\"{key}\":{}", json_string(v)));
      }
    }
    for (key, value) in [
      ("visible", self.visible),
      ("clipboardRead", self.clipboard_read),
      ("clipboardWrite", self.clipboard_write),
    ] {
      if let Some(v) = value {
        parts.push(format!("\"{key}\":{v}"));
      }
    }
    format!("{{{}}}", parts.join(","))
  }
}

/// A web view event's kind.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WebViewEventKind {
  Title,
  Url,
  Load,
  Console,
  FocusChange,
  NewWindow,
  Closed,
  FindResult,
  ContextMenu,
}

impl WebViewEventKind {
  fn from_raw(kind: i32) -> Option<Self> {
    Some(match kind as u32 {
      ffi::LAUFEY_WEBVIEW_EVENT_TITLE => WebViewEventKind::Title,
      ffi::LAUFEY_WEBVIEW_EVENT_URL => WebViewEventKind::Url,
      ffi::LAUFEY_WEBVIEW_EVENT_LOAD => WebViewEventKind::Load,
      ffi::LAUFEY_WEBVIEW_EVENT_CONSOLE => WebViewEventKind::Console,
      ffi::LAUFEY_WEBVIEW_EVENT_FOCUS => WebViewEventKind::FocusChange,
      ffi::LAUFEY_WEBVIEW_EVENT_NEW_WINDOW => WebViewEventKind::NewWindow,
      ffi::LAUFEY_WEBVIEW_EVENT_CLOSED => WebViewEventKind::Closed,
      ffi::LAUFEY_WEBVIEW_EVENT_FIND_RESULT => WebViewEventKind::FindResult,
      ffi::LAUFEY_WEBVIEW_EVENT_CONTEXT_MENU => WebViewEventKind::ContextMenu,
      _ => return None,
    })
  }

  /// `"title"`, `"url"`, `"load"`, `"console"`, `"focus-change"`,
  /// `"new-window"`, `"closed"`, `"find-result"` or `"context-menu"`.
  pub fn name(&self) -> &'static str {
    match self {
      WebViewEventKind::Title => "title",
      WebViewEventKind::Url => "url",
      WebViewEventKind::Load => "load",
      WebViewEventKind::Console => "console",
      WebViewEventKind::FocusChange => "focus-change",
      WebViewEventKind::NewWindow => "new-window",
      WebViewEventKind::Closed => "closed",
      WebViewEventKind::FindResult => "find-result",
      WebViewEventKind::ContextMenu => "context-menu",
    }
  }
}

/// One web view event; `detail_json` is the event's JSON object (see the
/// event kinds in laufey.h).
#[derive(Debug, Clone, PartialEq)]
pub struct WebViewEvent {
  pub web_view_id: u32,
  pub kind: WebViewEventKind,
  pub detail_json: String,
}

/// Why a web view call was refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WebViewError {
  Unsupported,
  NotAllowedUrl,
  Limit,
  NoWindow,
  Invalid,
  Closed,
  Failed,
  NotAllowed,
}

impl WebViewError {
  fn from_status(status: i32) -> Self {
    match status as u32 {
      ffi::LAUFEY_WEBVIEW_UNSUPPORTED => WebViewError::Unsupported,
      ffi::LAUFEY_WEBVIEW_NOT_ALLOWED_URL => WebViewError::NotAllowedUrl,
      ffi::LAUFEY_WEBVIEW_LIMIT => WebViewError::Limit,
      ffi::LAUFEY_WEBVIEW_NO_WINDOW => WebViewError::NoWindow,
      ffi::LAUFEY_WEBVIEW_INVALID => WebViewError::Invalid,
      ffi::LAUFEY_WEBVIEW_CLOSED => WebViewError::Closed,
      ffi::LAUFEY_WEBVIEW_NOT_ALLOWED => WebViewError::NotAllowed,
      _ => WebViewError::Failed,
    }
  }

  /// `"unsupported"`, `"not_allowed_url"`, `"limit"`, `"no_window"`,
  /// `"invalid"`, `"closed"`, `"failed"` or `"not_allowed"`.
  pub fn code(&self) -> &'static str {
    match self {
      WebViewError::Unsupported => "unsupported",
      WebViewError::NotAllowedUrl => "not_allowed_url",
      WebViewError::Limit => "limit",
      WebViewError::NoWindow => "no_window",
      WebViewError::Invalid => "invalid",
      WebViewError::Closed => "closed",
      WebViewError::Failed => "failed",
      WebViewError::NotAllowed => "not_allowed",
    }
  }
}

impl std::fmt::Display for WebViewError {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    f.write_str(self.code())
  }
}

impl std::error::Error for WebViewError {}

/// A failed [`WebView::evaluate`] / [`WebView::capture`]: `code` is one of
/// [`WebViewError::code`]'s, `"error"` (the script threw) or
/// `"not_serializable"` (no JSON for the value).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct WebViewFailure {
  pub code: &'static str,
  pub message: String,
}

fn failure_code(status: i32) -> &'static str {
  match status as u32 {
    ffi::LAUFEY_WEBVIEW_SCRIPT_ERROR => "error",
    ffi::LAUFEY_WEBVIEW_NOT_SERIALIZABLE => "not_serializable",
    _ => WebViewError::from_status(status).code(),
  }
}

/// The image format of [`WebView::capture`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CaptureFormat {
  Png,
  Jpeg,
}

type EventSink = Box<dyn Fn(WebViewEvent) + Send + Sync>;

unsafe extern "C" fn event_trampoline(
  user_data: *mut c_void,
  webview_id: u32,
  kind: i32,
  detail_json: *const c_char,
) {
  if user_data.is_null() {
    return;
  }
  let Some(kind) = WebViewEventKind::from_raw(kind) else {
    return;
  };
  let detail_json = if detail_json.is_null() {
    "{}".to_string()
  } else {
    unsafe { CStr::from_ptr(detail_json) }
      .to_string_lossy()
      .into_owned()
  };
  let event = WebViewEvent {
    web_view_id: webview_id,
    kind,
    detail_json,
  };
  if kind == WebViewEventKind::Closed {
    // SAFETY: the box made in create_webview_with; CLOSED is the last call
    // for this id, so the backend never passes `user_data` again.
    let sink = unsafe { Box::from_raw(user_data as *mut EventSink) };
    sink(event);
  } else {
    // SAFETY: alive until CLOSED.
    let sink = unsafe { &*(user_data as *const EventSink) };
    sink(event);
  }
}

/// Create a web view in window `window_id`; `on_event` receives its events
/// (any thread), [`WebViewEventKind::Closed`] last. Any thread.
pub fn create_webview<F>(
  window_id: u32,
  options: &WebViewOptions,
  on_event: F,
) -> Result<WebView, WebViewError>
where
  F: Fn(WebViewEvent) + Send + Sync + 'static,
{
  create_webview_with(api(), window_id, options, on_event)
}

fn create_webview_with<F>(
  api: &LaufeyBackendApi,
  window_id: u32,
  options: &WebViewOptions,
  on_event: F,
) -> Result<WebView, WebViewError>
where
  F: Fn(WebViewEvent) + Send + Sync + 'static,
{
  let Some(f) = api.create_webview else {
    return Err(WebViewError::Unsupported);
  };
  let Ok(json) = CString::new(options.to_json()) else {
    return Err(WebViewError::Invalid);
  };
  let sink: Box<EventSink> = Box::new(Box::new(on_event));
  let user_data = Box::into_raw(sink) as *mut c_void;
  let mut status: i32 = ffi::LAUFEY_WEBVIEW_FAILED as i32;
  let id = unsafe {
    f(
      api.backend_data,
      window_id,
      json.as_ptr(),
      Some(event_trampoline),
      user_data,
      &mut status,
    )
  };
  if id == 0 {
    // SAFETY: no id, so the backend keeps no reference to `user_data`.
    drop(unsafe { Box::from_raw(user_data as *mut EventSink) });
    let status = if status == ffi::LAUFEY_WEBVIEW_OK as i32 {
      ffi::LAUFEY_WEBVIEW_FAILED as i32
    } else {
      status
    };
    return Err(WebViewError::from_status(status));
  }
  Ok(WebView { id })
}

/// A web view, by id. Calls on a closed one are refused (`false`,
/// [`WebViewError::Closed`], `None`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct WebView {
  id: u32,
}

type EvalCallback = Box<dyn FnOnce(Result<String, WebViewFailure>) + Send>;
type CaptureCallback = Box<dyn FnOnce(Result<Vec<u8>, WebViewFailure>) + Send>;

unsafe extern "C" fn eval_trampoline(
  user_data: *mut c_void,
  status: i32,
  text: *const c_char,
) {
  if user_data.is_null() {
    return;
  }
  // SAFETY: the box made in evaluate_with; the backend fires it once.
  let cb = unsafe { Box::from_raw(user_data as *mut EvalCallback) };
  let text = if text.is_null() {
    String::new()
  } else {
    unsafe { CStr::from_ptr(text) }
      .to_string_lossy()
      .into_owned()
  };
  if status == ffi::LAUFEY_WEBVIEW_OK as i32 {
    cb(Ok(text));
  } else {
    cb(Err(WebViewFailure {
      code: failure_code(status),
      message: text,
    }));
  }
}

unsafe extern "C" fn capture_trampoline(
  user_data: *mut c_void,
  status: i32,
  data: *const u8,
  len: usize,
  error: *const c_char,
) {
  if user_data.is_null() {
    return;
  }
  // SAFETY: the box made in capture_with; the backend fires it once.
  let cb = unsafe { Box::from_raw(user_data as *mut CaptureCallback) };
  if status == ffi::LAUFEY_WEBVIEW_OK as i32 && !data.is_null() {
    // SAFETY: the backend's buffer, valid for this call.
    let bytes = unsafe { std::slice::from_raw_parts(data, len) }.to_vec();
    cb(Ok(bytes));
  } else {
    let message = if error.is_null() {
      String::new()
    } else {
      unsafe { CStr::from_ptr(error) }
        .to_string_lossy()
        .into_owned()
    };
    let status = if status == ffi::LAUFEY_WEBVIEW_OK as i32 {
      ffi::LAUFEY_WEBVIEW_FAILED as i32
    } else {
      status
    };
    cb(Err(WebViewFailure {
      code: failure_code(status),
      message,
    }));
  }
}

impl WebView {
  /// The web view with this id (as [`WebView::id`] / events report it).
  pub fn from_id(id: u32) -> Self {
    WebView { id }
  }

  pub fn id(&self) -> u32 {
    self.id
  }

  /// Move / resize it. False when closed or for a negative / non-finite
  /// value.
  pub fn set_bounds(&self, bounds: WebViewBounds) -> bool {
    let api = api();
    match api.webview_set_bounds {
      Some(f) => unsafe {
        f(
          api.backend_data,
          self.id,
          bounds.x,
          bounds.y,
          bounds.width,
          bounds.height,
        )
      },
      None => false,
    }
  }

  /// Show or hide it (a hidden view keeps its page).
  pub fn set_visible(&self, visible: bool) -> bool {
    let api = api();
    match api.webview_set_visible {
      Some(f) => unsafe { f(api.backend_data, self.id, visible) },
      None => false,
    }
  }

  /// Load `url` (http(s) only).
  pub fn navigate(&self, url: &str) -> Result<(), WebViewError> {
    navigate_with(api(), self.id, url)
  }

  fn command(&self, command: u32) -> bool {
    let api = api();
    match api.webview_command {
      Some(f) => unsafe { f(api.backend_data, self.id, command as i32) },
      None => false,
    }
  }

  pub fn back(&self) -> bool {
    self.command(ffi::LAUFEY_WEBVIEW_COMMAND_BACK)
  }

  pub fn forward(&self) -> bool {
    self.command(ffi::LAUFEY_WEBVIEW_COMMAND_FORWARD)
  }

  pub fn reload(&self, ignore_cache: bool) -> bool {
    self.command(if ignore_cache {
      ffi::LAUFEY_WEBVIEW_COMMAND_RELOAD_IGNORE_CACHE
    } else {
      ffi::LAUFEY_WEBVIEW_COMMAND_RELOAD
    })
  }

  pub fn stop(&self) -> bool {
    self.command(ffi::LAUFEY_WEBVIEW_COMMAND_STOP)
  }

  /// Give it the keyboard focus.
  pub fn focus(&self) -> bool {
    self.command(ffi::LAUFEY_WEBVIEW_COMMAND_FOCUS)
  }

  /// Above every other web view of its window.
  pub fn raise(&self) -> bool {
    self.command(ffi::LAUFEY_WEBVIEW_COMMAND_RAISE)
  }

  /// Below every other web view of its window.
  pub fn lower(&self) -> bool {
    self.command(ffi::LAUFEY_WEBVIEW_COMMAND_LOWER)
  }

  /// Its place among the window's web views (higher is above; equal ones
  /// keep their creation order). False where unsupported or closed.
  pub fn set_z_order(&self, z_order: i32) -> bool {
    let api = api();
    match api.webview_set_z_order {
      Some(f) => unsafe { f(api.backend_data, self.id, z_order) },
      None => false,
    }
  }

  /// Page zoom (1 = 100%, 0.25..5). False where unsupported or closed.
  pub fn set_zoom(&self, factor: f64) -> bool {
    let api = api();
    match api.webview_set_zoom {
      Some(f) => unsafe { f(api.backend_data, self.id, factor) },
      None => false,
    }
  }

  /// The page zoom, `None` when closed or unsupported.
  pub fn zoom(&self) -> Option<f64> {
    let api = api();
    let f = api.webview_get_zoom?;
    let z = unsafe { f(api.backend_data, self.id) };
    (z > 0.0).then_some(z)
  }

  /// Highlight the next (`forward`) or previous match of `text`; a
  /// [`WebViewEventKind::FindResult`] event reports the count.
  pub fn find(&self, text: &str, forward: bool, match_case: bool) -> bool {
    let api = api();
    let (Some(f), Ok(text)) = (api.webview_find, CString::new(text)) else {
      return false;
    };
    let mut flags = 0;
    if forward {
      flags |= ffi::LAUFEY_WEBVIEW_FIND_FORWARD;
    }
    if match_case {
      flags |= ffi::LAUFEY_WEBVIEW_FIND_MATCH_CASE;
    }
    unsafe { f(api.backend_data, self.id, text.as_ptr(), flags) }
  }

  /// Clear the find highlight.
  pub fn stop_find(&self) -> bool {
    let api = api();
    match api.webview_stop_find {
      Some(f) => unsafe { f(api.backend_data, self.id) },
      None => false,
    }
  }

  /// Open the engine's developer tools for this view.
  pub fn open_devtools(&self) -> Result<(), WebViewError> {
    let api = api();
    let Some(f) = api.webview_open_devtools else {
      return Err(WebViewError::Unsupported);
    };
    let status = unsafe { f(api.backend_data, self.id) };
    if status == ffi::LAUFEY_WEBVIEW_OK as i32 {
      Ok(())
    } else {
      Err(WebViewError::from_status(status))
    }
  }

  /// Print the page with the OS print dialog.
  pub fn print(&self) -> bool {
    let api = api();
    match api.webview_print {
      Some(f) => unsafe { f(api.backend_data, self.id) },
      None => false,
    }
  }

  /// Its state as JSON (`webview_state` in laufey.h); `None` when closed.
  pub fn state(&self) -> Option<String> {
    let api = api();
    let f = api.webview_state?;
    unsafe { take_backend_string(api, f(api.backend_data, self.id)) }
  }

  /// Run `script` in an isolated world of its page; `callback` gets the
  /// result as JSON, exactly once.
  pub fn evaluate<F>(&self, script: &str, callback: F)
  where
    F: FnOnce(Result<String, WebViewFailure>) + Send + 'static,
  {
    evaluate_with(api(), self.id, script, callback)
  }

  /// Capture what it shows; `callback` gets the image, exactly once.
  /// `scale` is output pixels per CSS pixel (`None`: device pixels).
  pub fn capture<F>(
    &self,
    format: CaptureFormat,
    quality: u8,
    scale: Option<f64>,
    callback: F,
  ) where
    F: FnOnce(Result<Vec<u8>, WebViewFailure>) + Send + 'static,
  {
    capture_with(api(), self.id, format, quality, scale, callback)
  }

  /// Close it ([`WebViewEventKind::Closed`] follows). False when already
  /// closed.
  pub fn close(&self) -> bool {
    let api = api();
    match api.webview_close {
      Some(f) => unsafe { f(api.backend_data, self.id) },
      None => false,
    }
  }
}

fn navigate_with(
  api: &LaufeyBackendApi,
  id: u32,
  url: &str,
) -> Result<(), WebViewError> {
  let Some(f) = api.webview_navigate else {
    return Err(WebViewError::Unsupported);
  };
  let Ok(url) = CString::new(url) else {
    return Err(WebViewError::NotAllowedUrl);
  };
  let status = unsafe { f(api.backend_data, id, url.as_ptr()) };
  if status == ffi::LAUFEY_WEBVIEW_OK as i32 {
    Ok(())
  } else {
    Err(WebViewError::from_status(status))
  }
}

fn evaluate_with<F>(api: &LaufeyBackendApi, id: u32, script: &str, callback: F)
where
  F: FnOnce(Result<String, WebViewFailure>) + Send + 'static,
{
  let Some(f) = api.webview_evaluate else {
    callback(Err(WebViewFailure {
      code: "unsupported",
      message: "this backend has no web views".into(),
    }));
    return;
  };
  let Ok(script) = CString::new(script) else {
    callback(Err(WebViewFailure {
      code: "invalid",
      message: "the script contains a NUL byte".into(),
    }));
    return;
  };
  let boxed: Box<EvalCallback> = Box::new(Box::new(callback));
  unsafe {
    f(
      api.backend_data,
      id,
      script.as_ptr(),
      Some(eval_trampoline),
      Box::into_raw(boxed) as *mut c_void,
    )
  };
}

fn capture_with<F>(
  api: &LaufeyBackendApi,
  id: u32,
  format: CaptureFormat,
  quality: u8,
  scale: Option<f64>,
  callback: F,
) where
  F: FnOnce(Result<Vec<u8>, WebViewFailure>) + Send + 'static,
{
  let Some(f) = api.webview_capture else {
    callback(Err(WebViewFailure {
      code: "unsupported",
      message: "this backend has no web views".into(),
    }));
    return;
  };
  let format = match format {
    CaptureFormat::Png => ffi::LAUFEY_WEBVIEW_CAPTURE_PNG,
    CaptureFormat::Jpeg => ffi::LAUFEY_WEBVIEW_CAPTURE_JPEG,
  };
  let boxed: Box<CaptureCallback> = Box::new(Box::new(callback));
  unsafe {
    f(
      api.backend_data,
      id,
      format as i32,
      quality.min(100) as i32,
      scale.filter(|s| s.is_finite() && *s > 0.0).unwrap_or(0.0),
      Some(capture_trampoline),
      Box::into_raw(boxed) as *mut c_void,
    )
  };
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::sync::mpsc;
  use std::sync::Mutex;

  static LAST_OPTIONS: Mutex<String> = Mutex::new(String::new());

  unsafe extern "C" fn fake_create(
    _: *mut c_void,
    window_id: u32,
    json: *const c_char,
    on_event: Option<
      unsafe extern "C" fn(*mut c_void, u32, i32, *const c_char),
    >,
    user_data: *mut c_void,
    status_out: *mut i32,
  ) -> u32 {
    *LAST_OPTIONS.lock().unwrap() = unsafe { CStr::from_ptr(json) }
      .to_string_lossy()
      .into_owned();
    if window_id == 0 {
      unsafe { *status_out = ffi::LAUFEY_WEBVIEW_NO_WINDOW as i32 };
      return 0;
    }
    unsafe { *status_out = ffi::LAUFEY_WEBVIEW_OK as i32 };
    let on_event = on_event.unwrap();
    let title = CString::new(r#"{"title":"T"}"#).unwrap();
    let closed = CString::new("{}").unwrap();
    unsafe {
      on_event(
        user_data,
        9,
        ffi::LAUFEY_WEBVIEW_EVENT_TITLE as i32,
        title.as_ptr(),
      );
      on_event(user_data, 9, 99, title.as_ptr());
      on_event(
        user_data,
        9,
        ffi::LAUFEY_WEBVIEW_EVENT_CLOSED as i32,
        closed.as_ptr(),
      );
    }
    9
  }

  #[test]
  fn capabilities_and_engine() {
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    let caps = webview_capabilities_with(&none);
    assert!(!caps.supported());
    assert_eq!(caps.engine(), None);
    let caps = WebViewCapabilities(
      ffi::LAUFEY_WEBVIEW_CAP_SUPPORTED
        | ffi::LAUFEY_WEBVIEW_CAP_CAPTURE
        | ffi::LAUFEY_WEBVIEW_CAP_PERSISTENT_PARTITIONS
        | (ffi::LAUFEY_WEBVIEW_ENGINE_WEBVIEW2
          << ffi::LAUFEY_WEBVIEW_ENGINE_SHIFT),
    );
    assert!(caps.supported() && caps.capture());
    assert!(!caps.evaluate() && !caps.permission_prompts());
    assert!(caps.persistent_partitions() && !caps.z_order());
    assert!(WebViewCapabilities(ffi::LAUFEY_WEBVIEW_CAP_Z_ORDER).z_order());
    assert_eq!(caps.engine(), Some(WebViewEngine::WebView2));
    assert_eq!(caps.engine().unwrap().name(), "webview2");
    // The header's numbers are the ABI.
    assert_eq!(ffi::LAUFEY_WEBVIEW_ENGINE_SHIFT, 24);
    assert_eq!(ffi::LAUFEY_WEBVIEW_ENGINE_CEF, 1);
    assert_eq!(ffi::LAUFEY_WEBVIEW_ENGINE_WEBKITGTK, 4);
  }

  #[test]
  fn options_json() {
    let opts = WebViewOptions {
      url: "https://a.example/\"q\"".into(),
      bounds: WebViewBounds {
        x: 1.0,
        y: 2.5,
        width: 300.0,
        height: f64::NAN,
      },
      partition: Some("ephemeral:x".into()),
      visible: Some(false),
      allow_popups: Some("deny".into()),
      downloads: None,
      permissions: Some("ask".into()),
      user_agent: Some("UA".into()),
      context_menu: Some("app".into()),
      clipboard_read: Some(true),
      clipboard_write: None,
    };
    assert_eq!(
      opts.to_json(),
      r#"{"url":"https://a.example/\"q\"","x":1,"y":2.5,"width":300,"height":null,"partition":"ephemeral:x","allowPopups":"deny","permissions":"ask","userAgent":"UA","contextMenu":"app","visible":false,"clipboardRead":true}"#
    );
  }

  #[test]
  fn create_routes_events_and_frees_the_sink_at_closed() {
    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    api.create_webview = Some(fake_create);
    let (tx, rx) = mpsc::channel();
    let tx = Mutex::new(tx);
    let view = create_webview_with(
      &api,
      3,
      &WebViewOptions {
        url: "https://a.example/".into(),
        ..Default::default()
      },
      move |e| {
        let _ = tx.lock().unwrap().send(e);
      },
    )
    .unwrap();
    assert_eq!(view.id(), 9);
    let events: Vec<WebViewEvent> = rx.iter().collect();
    assert_eq!(events.len(), 2, "the unknown kind is dropped");
    assert_eq!(events[0].kind, WebViewEventKind::Title);
    assert_eq!(events[0].detail_json, r#"{"title":"T"}"#);
    assert_eq!(events[1].kind.name(), "closed");
    // rx.iter() ended: the sink (and its sender) was dropped at CLOSED.
  }

  #[test]
  fn create_failure_maps_the_status() {
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    let opts = WebViewOptions::default();
    assert_eq!(
      create_webview_with(&none, 1, &opts, |_| {}),
      Err(WebViewError::Unsupported)
    );
    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    api.create_webview = Some(fake_create);
    let err = create_webview_with(&api, 0, &opts, |_| {}).unwrap_err();
    assert_eq!(err, WebViewError::NoWindow);
    assert_eq!(err.code(), "no_window");
  }

  unsafe extern "C" fn fake_navigate(
    _: *mut c_void,
    _: u32,
    url: *const c_char,
  ) -> i32 {
    let url = unsafe { CStr::from_ptr(url) }.to_str().unwrap();
    if url.starts_with("https:") {
      ffi::LAUFEY_WEBVIEW_OK as i32
    } else {
      ffi::LAUFEY_WEBVIEW_NOT_ALLOWED_URL as i32
    }
  }

  #[test]
  fn navigate_statuses() {
    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(
      navigate_with(&api, 1, "https://a/"),
      Err(WebViewError::Unsupported)
    );
    api.webview_navigate = Some(fake_navigate);
    assert_eq!(navigate_with(&api, 1, "https://a/"), Ok(()));
    assert_eq!(
      navigate_with(&api, 1, "file:///x"),
      Err(WebViewError::NotAllowedUrl)
    );
    assert_eq!(
      navigate_with(&api, 1, "https://a/\0"),
      Err(WebViewError::NotAllowedUrl)
    );
  }

  unsafe extern "C" fn fake_evaluate(
    _: *mut c_void,
    _: u32,
    script: *const c_char,
    cb: Option<unsafe extern "C" fn(*mut c_void, i32, *const c_char)>,
    ud: *mut c_void,
  ) {
    let script = unsafe { CStr::from_ptr(script) }.to_str().unwrap();
    let (status, text) = match script {
      "1+1" => (ffi::LAUFEY_WEBVIEW_OK, "2"),
      "f" => (ffi::LAUFEY_WEBVIEW_NOT_SERIALIZABLE, "a function"),
      _ => (ffi::LAUFEY_WEBVIEW_SCRIPT_ERROR, "Error: x"),
    };
    let text = CString::new(text).unwrap();
    unsafe { cb.unwrap()(ud, status as i32, text.as_ptr()) };
  }

  #[test]
  fn evaluate_results() {
    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    let (tx, rx) = mpsc::channel();
    let t = tx.clone();
    evaluate_with(&api, 1, "1+1", move |r| {
      let _ = t.send(r);
    });
    assert_eq!(rx.recv().unwrap().unwrap_err().code, "unsupported");
    api.webview_evaluate = Some(fake_evaluate);
    for (script, want) in [
      ("1+1", Ok("2".to_string())),
      (
        "f",
        Err(WebViewFailure {
          code: "not_serializable",
          message: "a function".into(),
        }),
      ),
      (
        "throw",
        Err(WebViewFailure {
          code: "error",
          message: "Error: x".into(),
        }),
      ),
    ] {
      let t = tx.clone();
      evaluate_with(&api, 1, script, move |r| {
        let _ = t.send(r);
      });
      assert_eq!(rx.recv().unwrap(), want);
    }
  }

  unsafe extern "C" fn fake_capture(
    _: *mut c_void,
    id: u32,
    format: i32,
    quality: i32,
    scale: f64,
    cb: Option<
      unsafe extern "C" fn(*mut c_void, i32, *const u8, usize, *const c_char),
    >,
    ud: *mut c_void,
  ) {
    if id == 0 {
      let msg = CString::new("closed").unwrap();
      unsafe {
        cb.unwrap()(
          ud,
          ffi::LAUFEY_WEBVIEW_CLOSED as i32,
          std::ptr::null(),
          0,
          msg.as_ptr(),
        )
      };
      return;
    }
    let bytes = [format as u8, quality as u8, scale as u8];
    unsafe {
      cb.unwrap()(
        ud,
        ffi::LAUFEY_WEBVIEW_OK as i32,
        bytes.as_ptr(),
        bytes.len(),
        std::ptr::null(),
      )
    };
  }

  #[test]
  fn capture_results() {
    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    api.webview_capture = Some(fake_capture);
    let (tx, rx) = mpsc::channel();
    let t = tx.clone();
    capture_with(&api, 1, CaptureFormat::Jpeg, 250, Some(2.0), move |r| {
      let _ = t.send(r);
    });
    assert_eq!(rx.recv().unwrap(), Ok(vec![1, 100, 2]));
    let t = tx.clone();
    capture_with(&api, 1, CaptureFormat::Png, 0, Some(f64::NAN), move |r| {
      let _ = t.send(r);
    });
    assert_eq!(rx.recv().unwrap(), Ok(vec![0, 0, 0]));
    capture_with(&api, 0, CaptureFormat::Png, 0, None, move |r| {
      let _ = tx.send(r);
    });
    let err = rx.recv().unwrap().unwrap_err();
    assert_eq!((err.code, err.message.as_str()), ("closed", "closed"));
  }

  #[test]
  fn event_kind_names_and_error_codes() {
    assert_eq!(
      WebViewEventKind::from_raw(ffi::LAUFEY_WEBVIEW_EVENT_FOCUS as i32)
        .map(|k| k.name()),
      Some("focus-change")
    );
    assert_eq!(
      WebViewEventKind::from_raw(ffi::LAUFEY_WEBVIEW_EVENT_NEW_WINDOW as i32)
        .map(|k| k.name()),
      Some("new-window")
    );
    assert_eq!(WebViewEventKind::from_raw(0), None);
    assert_eq!(WebViewError::from_status(3).code(), "limit");
    assert_eq!(WebViewError::from_status(2).code(), "not_allowed_url");
    assert_eq!(WebViewError::from_status(42).code(), "failed");
    assert_eq!(WebViewError::from_status(10).code(), "not_allowed");
    assert_eq!(
      WebViewEventKind::from_raw(ffi::LAUFEY_WEBVIEW_EVENT_FIND_RESULT as i32)
        .map(|k| k.name()),
      Some("find-result")
    );
    assert_eq!(
      WebViewEventKind::from_raw(ffi::LAUFEY_WEBVIEW_EVENT_CONTEXT_MENU as i32)
        .map(|k| k.name()),
      Some("context-menu")
    );
    assert_eq!(failure_code(8), "error");
    assert_eq!(failure_code(6), "closed");
  }
}
