// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Focus follows the window's activation (WebView2 on Windows), part of the
//! window-api checks (`--window-api`):
//!
//! - a real click in the page (SendInput) gives the keyboard focus to
//!   WebView2's child window while the window stays the active one: no blur
//!   (it used to fire one from the top-level's WM_KILLFOCUS, so a tray panel
//!   that hides on blur hid before the click reached the page);
//! - another window of the app taking the foreground does blur it.
//!
//! N/A on the other backends and OSes (their focus events come from the
//! toolkit's own activation), and where the click can't reach the window
//! (another window over it, no foreground rights in the session).

use crate::na;

pub async fn run() {
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  #[cfg(windows)]
  if backend == "webview" {
    imp::run().await;
    return;
  }
  na(&format!(
    "focus follows activation ({backend} on {}: WebView2 only)",
    std::env::consts::OS
  ));
}

#[cfg(windows)]
mod imp {
  use std::sync::{Arc, Mutex};

  use laufey::{Window, WindowOptions};

  use crate::io_checks::ole_input;
  use crate::{check, na, wait_for};

  type Hwnd = *mut std::ffi::c_void;

  #[repr(C)]
  struct Point {
    x: i32,
    y: i32,
  }

  #[repr(C)]
  struct Rect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
  }

  #[link(name = "user32")]
  extern "system" {
    // As auth_thread_checks.rs declares it (one signature per symbol).
    fn FindWindowW(class: *const u16, title: *const u16) -> isize;
    fn GetClientRect(hwnd: Hwnd, rect: *mut Rect) -> i32;
    fn ClientToScreen(hwnd: Hwnd, point: *mut Point) -> i32;
    fn GetForegroundWindow() -> Hwnd;
    fn GetAncestor(hwnd: Hwnd, flags: u32) -> Hwnd;
    fn GetGUIThreadInfo(thread: u32, info: *mut GuiThreadInfo) -> i32;
    fn GetClassNameW(hwnd: Hwnd, buf: *mut u16, len: i32) -> i32;
  }

  #[repr(C)]
  struct GuiThreadInfo {
    size: u32,
    flags: u32,
    active: Hwnd,
    focus: Hwnd,
    capture: Hwnd,
    menu_owner: Hwnd,
    move_size: Hwnd,
    caret: Hwnd,
    caret_rect: Rect,
  }

  /// The foreground thread's keyboard focus window, and its class name.
  fn keyboard_focus() -> (Hwnd, String) {
    let mut info = GuiThreadInfo {
      size: std::mem::size_of::<GuiThreadInfo>() as u32,
      flags: 0,
      active: std::ptr::null_mut(),
      focus: std::ptr::null_mut(),
      capture: std::ptr::null_mut(),
      menu_owner: std::ptr::null_mut(),
      move_size: std::ptr::null_mut(),
      caret: std::ptr::null_mut(),
      caret_rect: Rect {
        left: 0,
        top: 0,
        right: 0,
        bottom: 0,
      },
    };
    if unsafe { GetGUIThreadInfo(0, &mut info) } == 0 || info.focus.is_null() {
      return (std::ptr::null_mut(), String::new());
    }
    let mut class = [0u16; 128];
    let n = unsafe { GetClassNameW(info.focus, class.as_mut_ptr(), 128) };
    (
      info.focus,
      String::from_utf16_lossy(&class[..n.max(0) as usize]),
    )
  }

  fn hwnd_of(title: &str) -> Hwnd {
    let wide: Vec<u16> =
      title.encode_utf16().chain(std::iter::once(0)).collect();
    unsafe { FindWindowW(std::ptr::null(), wide.as_ptr()) as Hwnd }
  }

  /// The client area's centre in screen pixels.
  fn page_centre(hwnd: Hwnd) -> Option<(i32, i32)> {
    let mut rc = Rect {
      left: 0,
      top: 0,
      right: 0,
      bottom: 0,
    };
    if unsafe { GetClientRect(hwnd, &mut rc) } == 0 || rc.right <= 0 {
      return None;
    }
    let mut pt = Point {
      x: rc.right / 2,
      y: rc.bottom / 2,
    };
    if unsafe { ClientToScreen(hwnd, &mut pt) } == 0 {
      return None;
    }
    Some((pt.x, pt.y))
  }

  fn is_foreground(hwnd: Hwnd) -> bool {
    let fg = unsafe { GetForegroundWindow() };
    !fg.is_null() && unsafe { GetAncestor(fg, 2) } == hwnd // GA_ROOT
  }

  async fn click(x: i32, y: i32) {
    ole_input::move_to(x, y);
    tokio::time::sleep(std::time::Duration::from_millis(80)).await;
    ole_input::button(true);
    tokio::time::sleep(std::time::Duration::from_millis(60)).await;
    ole_input::button(false);
  }

  fn focus_window(title: &str, events: &Arc<Mutex<Vec<bool>>>) -> Window {
    let ev = events.clone();
    Window::new(360, 300)
      .title(title)
      .on_focused(move |e| ev.lock().unwrap().push(e.focused))
  }

  pub async fn run() {
    const TITLE: &str = "native-e2e-focus";
    let events = Arc::new(Mutex::new(Vec::<bool>::new()));
    let w = focus_window(TITLE, &events);
    w.show();
    w.focus();
    if !wait_for(|| w.get_size().0 != 0, 100, 50).await {
      check("focus window reports a size", false);
      return;
    }
    // Let the page load (WebView2 creates its child window then).
    tokio::time::sleep(std::time::Duration::from_millis(1500)).await;
    let hwnd = hwnd_of(TITLE);
    let Some((x, y)) = (!hwnd.is_null()).then(|| page_centre(hwnd)).flatten()
    else {
      na("focus follows activation (no HWND / client rect for the window)");
      w.close();
      return;
    };
    if !ole_input::ours_at(x, y) {
      na(&format!(
        "focus follows activation (the page centre is under {})",
        ole_input::window_at(x, y)
      ));
      w.close();
      return;
    }

    // 1. Clicks in the page: the window becomes / stays the active one and
    //    its focus moves into WebView2's child window. No blur.
    events.lock().unwrap().clear();
    click(x, y).await;
    tokio::time::sleep(std::time::Duration::from_millis(800)).await;
    click(x, y).await;
    tokio::time::sleep(std::time::Duration::from_millis(800)).await;
    if !is_foreground(hwnd) {
      na("focus follows activation (the click left another window active)");
      w.close();
      return;
    }
    // The click must have put the keyboard focus inside the page (one of
    // WebView2's child windows), else it proves nothing.
    let (focus, class) = keyboard_focus();
    let in_page = !focus.is_null()
      && focus != hwnd
      && unsafe { GetAncestor(focus, 2) } == hwnd;
    eprintln!(
      "[e2e] INFO keyboard focus after the page clicks: {class:?} (in the \
       page: {in_page})"
    );
    if !in_page {
      na(
        "focus follows activation (the click left the focus outside the page)",
      );
      w.close();
      return;
    }
    let seen = events.lock().unwrap().clone();
    eprintln!("[e2e] INFO focus events during the page clicks: {seen:?}");
    check(
      "a click in the page is no blur while the window stays active (R-W1)",
      !seen.contains(&false),
    );

    // 2. Another window taking the foreground blurs it.
    events.lock().unwrap().clear();
    let other_events = Arc::new(Mutex::new(Vec::<bool>::new()));
    const OTHER: &str = "native-e2e-focus-other";
    let other = focus_window(OTHER, &other_events);
    other.show();
    other.focus();
    let blurred =
      wait_for(|| events.lock().unwrap().contains(&false), 60, 50).await;
    let other_hwnd = hwnd_of(OTHER);
    if !other_hwnd.is_null() && is_foreground(other_hwnd) {
      check(
        "another window taking the foreground blurs the window",
        blurred,
      );
      check(
        "the window taking the foreground reports focus",
        other_events.lock().unwrap().contains(&true),
      );
    } else if blurred {
      check(
        "another window taking the foreground blurs the window",
        true,
      );
    } else {
      na(
        "blur on deactivation (the second window did not take the foreground)",
      );
    }

    // 3. A tray panel (non-activating, given the foreground by focus(), as
    //    Tray.attachPanel does): a click in it is no blur, so it doesn't hide
    //    before the click lands; another window taking over is one.
    const PANEL: &str = "native-e2e-focus-panel";
    let panel_events = Arc::new(Mutex::new(Vec::<bool>::new()));
    let ev = panel_events.clone();
    let panel = Window::new_with_options(
      300,
      200,
      WindowOptions {
        no_activate: true,
        ..WindowOptions::default()
      },
    )
    .title(PANEL)
    .on_focused(move |e| ev.lock().unwrap().push(e.focused));
    panel.set_position(60, 60);
    panel.show();
    panel.focus();
    tokio::time::sleep(std::time::Duration::from_millis(1500)).await;
    let panel_hwnd = hwnd_of(PANEL);
    let centre = (!panel_hwnd.is_null())
      .then(|| page_centre(panel_hwnd))
      .flatten();
    match centre {
      Some((px, py))
        if is_foreground(panel_hwnd) && ole_input::ours_at(px, py) =>
      {
        panel_events.lock().unwrap().clear();
        click(px, py).await;
        tokio::time::sleep(std::time::Duration::from_millis(800)).await;
        let seen = panel_events.lock().unwrap().clone();
        eprintln!("[e2e] INFO focus events of the panel click: {seen:?}");
        check(
          "a click in a non-activating panel is no blur",
          !seen.contains(&false),
        );
        other.focus();
        check(
          "another window taking the foreground blurs the panel",
          wait_for(|| panel_events.lock().unwrap().contains(&false), 60, 50)
            .await,
        );
      }
      _ => na("a non-activating panel (it did not take the foreground)"),
    }
    panel.close();
    other.close();
    w.close();
  }
}
