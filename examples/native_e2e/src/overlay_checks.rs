// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Overlay windows, tray events, the activation policy and text to speech
//! (API 50), part of the window-api checks (`--window-api`):
//!
//! - overlay_capabilities / tray_capabilities report what each backend and
//!   OS has (the table in docs/window-management.md, "Overlay windows");
//! - set_skip_taskbar / set_visible_on_all_workspaces / set_level answer
//!   what their capability says, and an unknown level is refused;
//! - an input region and explicit passthrough stay apart: passthrough reads
//!   back as set whatever the region does, and on Windows the region's
//!   cursor timer makes the window transparent to the mouse while the cursor
//!   is outside its rectangles (WS_EX_TRANSPARENT, read back from the HWND);
//! - macOS: the region's ignoresMouseEvents is current right after the
//!   window moves under a cursor at rest (no timer in between); with
//!   LAUFEY_E2E_POST_CLICKS=1 (it needs the Post Event permission) a click
//!   posted right after the move lands in the window or passes it by;
//! - macOS: the activation policy switches to accessory and back;
//! - speech: the voice list is a JSON array, and a silent utterance ends
//!   exactly once, DONE where the OS has an engine and UNSUPPORTED where it
//!   has none.

use std::sync::{Arc, Mutex};

use laufey::{
  InputRect, OverlayCapabilities, SpeechOutcome, SpeechRequest, Window,
};

use crate::{check, na, wait_for};

fn expected_caps(backend: &str) -> Option<u32> {
  use laufey::*;
  let all_mac = LAUFEY_OVERLAY_CAP_CLICK_PASSTHROUGH
    | LAUFEY_OVERLAY_CAP_PASSTHROUGH_FORWARD
    | LAUFEY_OVERLAY_CAP_INPUT_REGION
    | LAUFEY_OVERLAY_CAP_ALL_WORKSPACES
    | LAUFEY_OVERLAY_CAP_VISIBLE_ON_FULL_SCREEN
    | LAUFEY_OVERLAY_CAP_WINDOW_LEVELS
    | LAUFEY_OVERLAY_CAP_ACTIVATION_POLICY
    | LAUFEY_OVERLAY_CAP_NON_ACTIVATING;
  let win = LAUFEY_OVERLAY_CAP_CLICK_PASSTHROUGH
    | LAUFEY_OVERLAY_CAP_INPUT_REGION
    | LAUFEY_OVERLAY_CAP_SKIP_TASKBAR
    | LAUFEY_OVERLAY_CAP_WINDOW_LEVELS
    | LAUFEY_OVERLAY_CAP_NON_ACTIVATING;
  match (backend, std::env::consts::OS) {
    ("webview", "macos") => Some(all_mac | LAUFEY_OVERLAY_CAP_TRANSPARENT),
    ("cef", "macos") => Some(all_mac),
    ("webview", "windows") => Some(win | LAUFEY_OVERLAY_CAP_TRANSPARENT),
    ("cef", "windows") => Some(win),
    ("winit", _) => Some(0),
    // Linux depends on the session (X11 / Wayland, a compositor, layer
    // shell): checked bit by bit below.
    _ => None,
  }
}

// The window's top-level HWND, by its title: the WebView backend answers no
// native handle (get_window_handle is NULL there).
#[cfg(windows)]
fn is_mouse_transparent(title: &str) -> Option<bool> {
  #[link(name = "user32")]
  extern "system" {
    fn FindWindowW(
      class: *const u16,
      title: *const u16,
    ) -> *mut std::ffi::c_void;
    fn GetWindowLongW(hwnd: *mut std::ffi::c_void, index: i32) -> i32;
  }
  const GWL_EXSTYLE: i32 = -20;
  const WS_EX_TRANSPARENT: i32 = 0x20;
  let wide: Vec<u16> = title.encode_utf16().chain(std::iter::once(0)).collect();
  let hwnd = unsafe { FindWindowW(std::ptr::null(), wide.as_ptr()) };
  if hwnd.is_null() {
    return None;
  }
  Some(unsafe { GetWindowLongW(hwnd, GWL_EXSTYLE) } & WS_EX_TRANSPARENT != 0)
}

pub async fn run(w: &Window, title: &str) {
  // Only the Windows passthrough probe looks the window up by its title.
  #[cfg(not(windows))]
  let _ = title;
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  let caps: OverlayCapabilities = laufey::overlay_capabilities();
  eprintln!("[e2e] overlay capabilities = {:#x}", caps.bits());
  match expected_caps(&backend) {
    Some(want) => check(
      &format!(
        "overlay_capabilities is {want:#x} for {backend} on {} (got {:#x})",
        std::env::consts::OS,
        caps.bits()
      ),
      caps.bits() == want,
    ),
    None => {
      // Linux: passthrough, regions and levels everywhere; skip-taskbar and
      // sticky only on X11; layer shell only on a Wayland compositor with it.
      let wayland = std::env::var("WAYLAND_DISPLAY")
        .is_ok_and(|v| !v.is_empty())
        && std::env::var("GDK_BACKEND").as_deref() != Ok("x11");
      check(
        "Linux: click passthrough and input regions are reported",
        caps.click_passthrough() && caps.input_region(),
      );
      check(
        "Linux: skip-taskbar and all-workspaces follow the session (X11 only)",
        caps.skip_taskbar() == !wayland && caps.all_workspaces() == !wayland,
      );
      check(
        "Linux: no layer shell under X11",
        wayland || !caps.layer_shell(),
      );
    }
  }

  // -- setters answer what their capability says ---------------------------
  check(
    "set_skip_taskbar(true) answers the skip-taskbar capability",
    w.set_skip_taskbar(true) == caps.skip_taskbar(),
  );
  w.set_skip_taskbar(false);
  check(
    "set_visible_on_all_workspaces answers the all-workspaces capability",
    w.set_visible_on_all_workspaces(true, true) == caps.all_workspaces(),
  );
  w.set_visible_on_all_workspaces(false, false);
  for level in [
    laufey::LAUFEY_WINDOW_LEVEL_FLOATING,
    laufey::LAUFEY_WINDOW_LEVEL_STATUS,
    laufey::LAUFEY_WINDOW_LEVEL_POP_UP_MENU,
    laufey::LAUFEY_WINDOW_LEVEL_SCREEN_SAVER,
  ] {
    check(
      &format!("set_level({level}) answers the window-levels capability"),
      w.set_level(level) == caps.window_levels(),
    );
  }
  check("set_level(99) is refused", !w.set_level(99));
  w.set_level(laufey::LAUFEY_WINDOW_LEVEL_NORMAL);

  // -- input region and passthrough stay apart ------------------------------
  if caps.input_region() {
    w.set_input_region(Some(&[InputRect {
      x: 0,
      y: 0,
      width: 10,
      height: 10,
    }]));
    w.set_click_passthrough(true);
    w.set_click_passthrough(false);
    // Read back: explicit passthrough only, whatever the region toggled.
    let off = wait_for(|| !w.get_click_passthrough(), 40, 50).await;
    check(
      "an input region doesn't read back as click passthrough",
      off,
    );
    #[cfg(windows)]
    {
      // An empty region: the cursor is outside it wherever it is, so the
      // timer makes the window transparent to the mouse...
      w.set_input_region(Some(&[]));
      let through =
        wait_for(|| is_mouse_transparent(title) == Some(true), 40, 50).await;
      check(
        "Windows: outside the input region the window is WS_EX_TRANSPARENT",
        through,
      );
      // ...and removing the region gives the window its input back.
      w.set_input_region(None);
      let back =
        wait_for(|| is_mouse_transparent(title) == Some(false), 40, 50).await;
      check(
        "Windows: without a region the window takes the mouse again",
        back,
      );
    }
    #[cfg(target_os = "macos")]
    mac_region_follows_moves(w, title).await;
    w.set_input_region(None);
  } else {
    na(&format!("input regions ({backend} has none here)"));
  }

  // -- the activation policy (macOS) ----------------------------------------
  if caps.activation_policy() {
    check(
      "the activation policy starts regular (no launch key, no LSUIElement)",
      laufey::activation_policy()
        == Some(laufey::LAUFEY_ACTIVATION_POLICY_REGULAR),
    );
    check(
      "set_activation_policy(accessory) is accepted",
      laufey::set_activation_policy(laufey::LAUFEY_ACTIVATION_POLICY_ACCESSORY),
    );
    let accessory = wait_for(
      || {
        laufey::activation_policy()
          == Some(laufey::LAUFEY_ACTIVATION_POLICY_ACCESSORY)
      },
      40,
      50,
    )
    .await;
    check("the activation policy reads back accessory", accessory);
    laufey::set_activation_policy(laufey::LAUFEY_ACTIVATION_POLICY_REGULAR);
    let regular = wait_for(
      || {
        laufey::activation_policy()
          == Some(laufey::LAUFEY_ACTIVATION_POLICY_REGULAR)
      },
      40,
      50,
    )
    .await;
    check("the activation policy reads back regular again", regular);
    check(
      "an unknown activation policy is refused",
      !laufey::set_activation_policy(7),
    );
  } else {
    check(
      "no activation policy: none is reported and setting it is refused",
      laufey::activation_policy().is_none()
        && !laufey::set_activation_policy(1),
    );
  }

  // -- tray capabilities ----------------------------------------------------
  let tray_caps = laufey::tray_capabilities();
  eprintln!("[e2e] tray capabilities = {tray_caps:#x}");
  if matches!(std::env::consts::OS, "macos" | "windows") && backend != "winit" {
    check(
      "tray events: every kind, modifiers, position and pop-up on macOS / Windows",
      tray_caps == 0xff,
    );
  }

  // -- text to speech -------------------------------------------------------
  if backend == "winit" {
    na("speech (Winit has none)");
    return;
  }
  let voices = laufey::speech_voices_json();
  check(
    "speech_voices is a JSON array",
    voices.starts_with('[') && voices.ends_with(']'),
  );
  let outcomes = Arc::new(Mutex::new(Vec::<SpeechOutcome>::new()));
  let o = outcomes.clone();
  let id = laufey::speak(
    &SpeechRequest {
      text: "laufey".into(),
      volume: Some(0.0),
      rate: Some(4.0),
      ..Default::default()
    },
    move |outcome| o.lock().unwrap().push(outcome),
  );
  check("speak returns an utterance id", id != 0);
  let ended = wait_for(|| !outcomes.lock().unwrap().is_empty(), 300, 50).await;
  let got = outcomes.lock().unwrap().clone();
  eprintln!("[e2e] speech outcome {got:?} (voices {voices})");
  check("a silent utterance ends", ended);
  // A second report would land in the list a moment later.
  tokio::time::sleep(std::time::Duration::from_millis(300)).await;
  check("it ends exactly once", outcomes.lock().unwrap().len() == 1);
  let engine = matches!(std::env::consts::OS, "macos" | "windows");
  if engine {
    // A CI runner may have no audio output device: SAPI then refuses the
    // utterance, which the backend reports as such.
    let no_audio = matches!(got.first(),
      Some(SpeechOutcome::Failed(m)) if m.contains("no audio output"));
    check(
      &format!(
        "macOS / Windows speak it (DONE{})",
        if no_audio {
          "; no audio output on this machine"
        } else {
          ""
        }
      ),
      got.first() == Some(&SpeechOutcome::Done) || no_audio,
    );
  } else {
    check(
      "Linux: DONE with speech-dispatcher, else UNSUPPORTED",
      matches!(
        got.first(),
        Some(SpeechOutcome::Done | SpeechOutcome::Unsupported)
      ),
    );
  }
  check(
    "an empty text is refused (no utterance)",
    laufey::speak(&SpeechRequest::default(), |_| {}) == 0,
  );
}

/// macOS: the input region is evaluated again the moment the window moves,
/// so the window server routes a click that comes right after the move by
/// where the window is now. It used to wait for a 30 Hz timer: for up to
/// 33 ms after a move the window still took (or passed) clicks by where it
/// had been.
#[cfg(target_os = "macos")]
async fn mac_region_follows_moves(w: &Window, title: &str) {
  // The cursor over the content's top-left corner, then a region there:
  // set_input_region evaluates it for the cursor where it now is (a warp
  // posts no mouse event for the monitors to see).
  let (cx, cy) = w.get_inner_position();
  let (fx, fy) = w.get_position();
  mac::warp_cursor(cx as f64 + 20.0, cy as f64 + 20.0);
  w.set_input_region(Some(&[InputRect {
    x: 0,
    y: 0,
    width: 40,
    height: 40,
  }]));
  let t = title.to_string();
  let over = wait_for_ui(move || mac::ignores_mouse(&t) == Some(false)).await;
  check(
    "macOS: over a region rectangle the window takes the mouse",
    over,
  );
  if !over {
    return;
  }
  let t = title.to_string();
  let (moved_away, moved_back) = laufey::spawn_on_ui_thread(move || {
    // Both reads in the same main-thread turn as the move: no timer can
    // have run in between.
    mac::move_window(&t, fx as f64 + 300.0, fy as f64);
    let away = mac::ignores_mouse(&t);
    mac::move_window(&t, fx as f64, fy as f64);
    (away, mac::ignores_mouse(&t))
  })
  .await
  .unwrap_or((None, None));
  check(
    "macOS: moved out from under the cursor, the window passes clicks at once",
    moved_away == Some(true),
  );
  check(
    "macOS: moved back under the cursor, it takes them again at once",
    moved_back == Some(false),
  );

  if std::env::var("LAUFEY_E2E_POST_CLICKS").as_deref() != Ok("1") {
    na("macOS: real clicks after a move (LAUFEY_E2E_POST_CLICKS=1)");
    return;
  }
  // Real clicks, posted 10 ms after each move (the 30 Hz timer this
  // replaced left the old state for up to 33 ms), counted by the page.
  if crate::page_number(
    w,
    "window.__downs = 0; \
     addEventListener('mousedown', () => __downs++, true); 0",
  )
  .await
  .is_none()
  {
    na("macOS: real clicks after a move (the page doesn't answer)");
    return;
  }
  // LAUFEY_E2E_CLICK_DELAY_MS changes the 10 ms (a sweep by hand).
  let delay = std::env::var("LAUFEY_E2E_CLICK_DELAY_MS")
    .ok()
    .and_then(|v| v.parse::<u64>().ok())
    .unwrap_or(10);
  let move_then_click = |x: f64| {
    let t = title.to_string();
    async move {
      let _ =
        laufey::spawn_on_ui_thread(move || mac::move_window(&t, x, fy as f64))
          .await;
      tokio::time::sleep(std::time::Duration::from_millis(delay)).await;
      mac::post_click_at_cursor();
      tokio::time::sleep(std::time::Duration::from_millis(250)).await;
    }
  };
  let mut landed = 0;
  let mut passed = 0;
  const ROUNDS: u32 = 10;
  for _ in 0..ROUNDS {
    // Away (the cursor outside the window), then back under the cursor.
    let t = title.to_string();
    let _ = laufey::spawn_on_ui_thread(move || {
      mac::move_window(&t, fx as f64 + 300.0, fy as f64)
    })
    .await;
    // The app active and the window key first: a click that passed went to
    // another app, and the first click on an inactive app's window only
    // activates it (the page never sees it).
    w.focus();
    tokio::time::sleep(std::time::Duration::from_millis(300)).await;
    let before = crate::page_number(w, "__downs").await.unwrap_or(-1.0);
    move_then_click(fx as f64).await;
    if crate::page_number(w, "__downs").await.unwrap_or(-1.0) > before {
      landed += 1;
    }
    // 60 px to the left: the cursor is still over the window, now beside
    // the rectangle.
    w.focus();
    tokio::time::sleep(std::time::Duration::from_millis(300)).await;
    let before = crate::page_number(w, "__downs").await.unwrap_or(-1.0);
    move_then_click(fx as f64 - 60.0).await;
    if crate::page_number(w, "__downs").await.unwrap_or(-1.0) == before {
      passed += 1;
    }
    let t = title.to_string();
    let _ = laufey::spawn_on_ui_thread(move || {
      mac::move_window(&t, fx as f64, fy as f64)
    })
    .await;
    tokio::time::sleep(std::time::Duration::from_millis(100)).await;
  }
  eprintln!(
    "[e2e] clicks {delay} ms after a move: {landed}/{ROUNDS} landed over the \
     region, {passed}/{ROUNDS} passed beside it"
  );
  check(
    "macOS: a click right after the window moves under the cursor lands",
    landed == ROUNDS,
  );
  check(
    "macOS: a click right after the region moves off the cursor passes",
    passed == ROUNDS,
  );
}

#[cfg(target_os = "macos")]
async fn wait_for_ui(
  f: impl Fn() -> bool + Send + Sync + Clone + 'static,
) -> bool {
  for _ in 0..40 {
    let g = f.clone();
    if laufey::spawn_on_ui_thread(g).await.unwrap_or(false) {
      return true;
    }
    tokio::time::sleep(std::time::Duration::from_millis(50)).await;
  }
  false
}

#[cfg(target_os = "macos")]
mod mac {
  //! The window by its title through the Objective-C runtime, the cursor
  //! and clicks through Quartz. Main thread unless noted.
  use std::ffi::{c_char, c_void, CStr};

  #[repr(C)]
  #[derive(Clone, Copy)]
  struct CGPoint {
    x: f64,
    y: f64,
  }
  #[repr(C)]
  #[derive(Clone, Copy)]
  struct CGSize {
    width: f64,
    height: f64,
  }
  #[repr(C)]
  #[derive(Clone, Copy)]
  struct CGRect {
    origin: CGPoint,
    size: CGSize,
  }

  #[link(name = "objc")]
  extern "C" {
    fn objc_getClass(name: *const c_char) -> *mut c_void;
    fn sel_registerName(name: *const c_char) -> *mut c_void;
    fn objc_msgSend();
  }

  #[link(name = "CoreGraphics", kind = "framework")]
  extern "C" {
    fn CGMainDisplayID() -> u32;
    fn CGDisplayBounds(display: u32) -> CGRect;
    fn CGWarpMouseCursorPosition(point: CGPoint) -> i32;
    fn CGEventCreate(source: *const c_void) -> *mut c_void;
    fn CGEventGetLocation(event: *mut c_void) -> CGPoint;
    fn CGEventCreateMouseEvent(
      source: *const c_void,
      kind: u32,
      point: CGPoint,
      button: u32,
    ) -> *mut c_void;
    fn CGEventPost(tap: u32, event: *mut c_void);
  }

  #[link(name = "CoreFoundation", kind = "framework")]
  extern "C" {
    fn CFRelease(cf: *const c_void);
  }

  const KCG_EVENT_LEFT_MOUSE_DOWN: u32 = 1;
  const KCG_EVENT_LEFT_MOUSE_UP: u32 = 2;
  const KCG_HID_EVENT_TAP: u32 = 0;

  unsafe fn send0(obj: *mut c_void, sel: &CStr) -> *mut c_void {
    let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void =
      std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel_registerName(sel.as_ptr()))
  }

  unsafe fn send_usize(obj: *mut c_void, sel: &CStr) -> usize {
    let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> usize =
      std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel_registerName(sel.as_ptr()))
  }

  unsafe fn send_bool(obj: *mut c_void, sel: &CStr) -> bool {
    let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool =
      std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel_registerName(sel.as_ptr()))
  }

  unsafe fn send_index(obj: *mut c_void, sel: &CStr, i: usize) -> *mut c_void {
    let f: unsafe extern "C" fn(
      *mut c_void,
      *mut c_void,
      usize,
    ) -> *mut c_void =
      std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel_registerName(sel.as_ptr()), i)
  }

  unsafe fn send_point(obj: *mut c_void, sel: &CStr, p: CGPoint) {
    let f: unsafe extern "C" fn(*mut c_void, *mut c_void, CGPoint) =
      std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel_registerName(sel.as_ptr()), p)
  }

  fn window_titled(title: &str) -> Option<*mut c_void> {
    unsafe {
      let app = send0(
        objc_getClass(c"NSApplication".as_ptr()),
        c"sharedApplication",
      );
      let windows = send0(app, c"windows");
      for i in 0..send_usize(windows, c"count") {
        let window = send_index(windows, c"objectAtIndex:", i);
        let name = send0(window, c"title");
        let utf8 = send0(name, c"UTF8String") as *const c_char;
        if !utf8.is_null()
          && CStr::from_ptr(utf8).to_bytes() == title.as_bytes()
        {
          return Some(window);
        }
      }
      None
    }
  }

  /// `-[NSWindow ignoresMouseEvents]` of the window titled `title`.
  pub fn ignores_mouse(title: &str) -> Option<bool> {
    let window = window_titled(title)?;
    Some(unsafe { send_bool(window, c"ignoresMouseEvents") })
  }

  /// Moves the window's frame top-left to (x, y), top-left screen
  /// coordinates (laufey's), with `-[NSWindow setFrameTopLeftPoint:]`.
  pub fn move_window(title: &str, x: f64, y: f64) {
    let Some(window) = window_titled(title) else {
      return;
    };
    let height = unsafe { CGDisplayBounds(CGMainDisplayID()) }.size.height;
    unsafe {
      send_point(
        window,
        c"setFrameTopLeftPoint:",
        CGPoint { x, y: height - y },
      )
    };
  }

  /// Moves the cursor (top-left screen coordinates). Any thread.
  pub fn warp_cursor(x: f64, y: f64) {
    unsafe { CGWarpMouseCursorPosition(CGPoint { x, y }) };
  }

  /// Posts a left click where the cursor is (needs the Post Event
  /// permission; without it the events are dropped). Any thread.
  pub fn post_click_at_cursor() {
    unsafe {
      let here = CGEventCreate(std::ptr::null());
      let at = CGEventGetLocation(here);
      CFRelease(here);
      for kind in [KCG_EVENT_LEFT_MOUSE_DOWN, KCG_EVENT_LEFT_MOUSE_UP] {
        let e = CGEventCreateMouseEvent(std::ptr::null(), kind, at, 0);
        CGEventPost(KCG_HID_EVENT_TAP, e);
        CFRelease(e);
      }
    }
  }
}
