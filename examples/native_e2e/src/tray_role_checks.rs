// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! A Windows tray menu's role items (LAUFEY_E2E_ONLY=tray-roles,
//! `--tray-roles`; it ends the process, so it runs on its own):
//!
//! - the menu as Windows draws it: a role gets its label (Linux's), the
//!   macOS-only roles are left out (they used to be blank rows that did
//!   nothing);
//! - choosing the quit role (a real click on its row, SendInput) quits the
//!   app as quit() does: the event loop ends, the runtime is shut down, and
//!   the menu's click handler never hears of it.
//!
//! N/A off Windows and on the Winit backend (no tray menu of tray_win.cc).

use crate::na;

pub async fn run() {
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  #[cfg(windows)]
  if backend == "webview" || backend == "cef" {
    imp::run().await;
    return;
  }
  na(&format!(
    "tray menu roles ({backend} on {}: Windows webview / cef only)",
    std::env::consts::OS
  ));
  crate::finish();
}

#[cfg(windows)]
mod imp {
  use std::ffi::c_void;
  use std::sync::{Arc, Mutex};
  use std::time::Duration;

  use laufey::{MenuItem, TrayIcon, Window};

  use crate::io_checks::ole_input;
  use crate::{check, wait_for};

  type Hwnd = *mut c_void;

  #[repr(C)]
  #[derive(Default)]
  struct Rect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
  }

  #[link(name = "user32")]
  extern "system" {
    fn EnumWindows(
      cb: unsafe extern "system" fn(Hwnd, isize) -> i32,
      lparam: isize,
    ) -> i32;
    fn GetWindowThreadProcessId(hwnd: Hwnd, pid: *mut u32) -> u32;
    fn IsWindowVisible(hwnd: Hwnd) -> i32;
    fn GetClassNameW(hwnd: Hwnd, buf: *mut u16, len: i32) -> i32;
    fn PostMessageW(hwnd: Hwnd, msg: u32, wp: usize, lp: isize) -> i32;
    fn SendMessageW(hwnd: Hwnd, msg: u32, wp: usize, lp: isize) -> isize;
    fn GetMenuItemCount(menu: Hwnd) -> i32;
    fn GetMenuStringW(
      menu: Hwnd,
      item: u32,
      buf: *mut u16,
      len: i32,
      flags: u32,
    ) -> i32;
    fn GetMenuItemRect(
      hwnd: Hwnd,
      menu: Hwnd,
      item: u32,
      rect: *mut Rect,
    ) -> i32;
  }

  /// WM_LAUFEY_COMMON_TRAYICON (tray_win.cc), and a right click in it.
  const WM_TRAYICON: u32 = 0x8000 + 65;
  const WM_RBUTTONUP: isize = 0x0205;
  const WM_CANCELMODE: u32 = 0x001F;
  const MN_GETHMENU: u32 = 0x01E1;
  const MF_BYPOSITION: u32 = 0x0400;

  /// This process's first visible top-level window of `class`, or null.
  fn window_of_class(class: &str, visible: bool) -> Hwnd {
    struct Search {
      pid: u32,
      class: Vec<u16>,
      visible: bool,
      found: Hwnd,
    }
    unsafe extern "system" fn visit(hwnd: Hwnd, lparam: isize) -> i32 {
      let s = &mut *(lparam as *mut Search);
      let mut pid = 0u32;
      GetWindowThreadProcessId(hwnd, &mut pid);
      if pid != s.pid || (s.visible && IsWindowVisible(hwnd) == 0) {
        return 1;
      }
      let mut buf = [0u16; 64];
      let n = GetClassNameW(hwnd, buf.as_mut_ptr(), buf.len() as i32);
      if n > 0 && buf[..n as usize] == s.class[..] {
        s.found = hwnd;
        return 0;
      }
      1
    }
    let mut s = Search {
      pid: std::process::id(),
      class: class.encode_utf16().collect(),
      visible,
      found: std::ptr::null_mut(),
    };
    unsafe { EnumWindows(visit, &mut s as *mut Search as isize) };
    s.found
  }

  /// The open popup menu's window and its HMENU's item labels.
  fn open_menu() -> Option<(Hwnd, Hwnd, Vec<String>)> {
    let popup = window_of_class("#32768", true);
    if popup.is_null() {
      return None;
    }
    let menu = unsafe { SendMessageW(popup, MN_GETHMENU, 0, 0) } as Hwnd;
    if menu.is_null() {
      return None;
    }
    let count = unsafe { GetMenuItemCount(menu) };
    let labels = (0..count.max(0) as u32)
      .map(|i| {
        let mut buf = [0u16; 128];
        let n = unsafe {
          GetMenuStringW(menu, i, buf.as_mut_ptr(), 128, MF_BYPOSITION)
        };
        String::from_utf16_lossy(&buf[..n.max(0) as usize])
      })
      .collect();
    Some((popup, menu, labels))
  }

  pub async fn run() {
    // A window, as an app has one (the quit must end the loop with it open).
    let w = Window::new(300, 200).title("native-e2e-tray-roles");
    let _ = wait_for(|| w.get_size().0 != 0, 100, 50).await;

    let clicks = Arc::new(Mutex::new(Vec::<String>::new()));
    let tray = TrayIcon::new();
    if tray.id() == 0 {
      check("tray created for the tray-role checks", false);
      crate::finish();
    }
    tray.set_icon(crate::TINY_PNG);
    let seen = clicks.clone();
    let role = |r: &str| MenuItem::Role { role: r.into() };
    tray.set_menu(
      &[
        MenuItem::Item {
          label: "Ping".into(),
          id: Some("tray_ping".into()),
          accelerator: None,
          enabled: true,
          checked: false,
          icon: None,
          tooltip: None,
          destructive: false,
        },
        role("hide"),
        role("front"),
        role("copy"),
        MenuItem::Separator,
        role("quit"),
      ],
      move |id| seen.lock().unwrap().push(id.to_string()),
    );
    let _ = wait_for(
      || !window_of_class("LaufeyCommonTrayWindow", false).is_null(),
      100,
      50,
    )
    .await;
    let tray_hwnd = window_of_class("LaufeyCommonTrayWindow", false);
    check("the tray window exists", !tray_hwnd.is_null());

    // Open the menu as a right click on the icon does (re-posted until the
    // menu is set and up).
    let mut menu = None;
    for _ in 0..10 {
      unsafe {
        PostMessageW(tray_hwnd, WM_TRAYICON, tray.id() as usize, WM_RBUTTONUP)
      };
      if wait_for(|| open_menu().is_some(), 20, 50).await {
        tokio::time::sleep(Duration::from_millis(300)).await;
        menu = open_menu();
        break;
      }
    }
    let Some((_popup, hmenu, labels)) = menu else {
      check("the tray menu is up", false);
      laufey::quit();
      crate::finish();
    };
    eprintln!("[e2e] INFO tray menu rows: {labels:?}");
    check(
      "role rows get their labels; macOS-only roles are left out \
       (Ping, Copy, separator, Quit)",
      labels == ["Ping", "Copy", "", "Quit"],
    );

    // Click the Quit row.
    let mut rect = Rect::default();
    let quit_row = (labels.len() - 1) as u32;
    let placed = unsafe {
      GetMenuItemRect(std::ptr::null_mut(), hmenu, quit_row, &mut rect)
    } != 0;
    check("the Quit row has a place on screen", placed);
    if placed {
      let (x, y) = ((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
      ole_input::move_to(x, y);
      tokio::time::sleep(Duration::from_millis(150)).await;
      ole_input::button(true);
      tokio::time::sleep(Duration::from_millis(60)).await;
      ole_input::button(false);
    }
    let ended = wait_for(laufey::should_shutdown, 200, 50).await;
    check(
      "choosing the quit role quits the app (the event loop ends)",
      ended,
    );
    check(
      "the quit role does not reach the menu's click handler",
      clicks.lock().unwrap().is_empty(),
    );
    if !ended {
      unsafe { PostMessageW(tray_hwnd, WM_CANCELMODE, 0, 0) };
      laufey::quit();
      let _ = wait_for(laufey::should_shutdown, 200, 50).await;
    }
    drop(tray);
    // Return: the backend joins this thread and ends the process.
    let failed = crate::FAILED.load(std::sync::atomic::Ordering::SeqCst);
    eprintln!("[e2e] OVERALL {}", if failed { "FAIL" } else { "PASS" });
    let _ = std::io::Write::flush(&mut std::io::stderr());
    if failed {
      unsafe { crate::libc_exit(1) };
    }
  }
}
