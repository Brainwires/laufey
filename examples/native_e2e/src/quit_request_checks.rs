//! The OS asks the app to quit (API 52, LAUFEY_E2E_ONLY=quit-request,
//! `--quit-request`; the last step ends the process, so it runs on its own).
//!
//! - macOS: `terminate:` asks the handler. A refusal keeps the app running, an
//!   answer that allows it ends the app, and a runtime that never answers
//!   can't hold it past the deadline.
//! - Windows: WM_QUERYENDSESSION to the hidden top-level window asks the
//!   handler as a session end. A refusal answers FALSE; no answer answers TRUE
//!   within the deadline, under the 5 s that makes Windows show its screen; a
//!   critical end asks nobody; WM_ENDSESSION quits.
//!
//! N/A on Linux and the Winit backends, which get no such request.

use crate::na;

pub async fn run() {
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  #[cfg(any(target_os = "macos", windows))]
  if backend == "webview" || backend == "cef" {
    imp::run().await;
    return;
  }
  na(&format!(
    "quit requests ({backend} on {}: macOS / Windows webview and cef only)",
    std::env::consts::OS
  ));
  crate::finish();
}

#[cfg(any(target_os = "macos", windows))]
mod imp {
  use std::sync::atomic::{AtomicI32, AtomicU32, Ordering};
  use std::time::{Duration, Instant};

  use laufey::{QuitRequestKind, Window};

  use crate::{check, wait_for};
  #[cfg(target_os = "macos")]
  use crate::na;

  /// What the handler does: answer with this, or (-1) stay silent.
  static ANSWER: AtomicI32 = AtomicI32::new(0);
  static ASKED: AtomicU32 = AtomicU32::new(0);
  static SESSION_END: AtomicU32 = AtomicU32::new(0);

  fn install() {
    laufey::on_quit_requested(|kind| {
      ASKED.fetch_add(1, Ordering::SeqCst);
      if kind == QuitRequestKind::SessionEnd {
        SESSION_END.fetch_add(1, Ordering::SeqCst);
      }
      match ANSWER.load(Ordering::SeqCst) {
        0 => laufey::answer_quit_request(false),
        1 => laufey::answer_quit_request(true),
        _ => {}
      }
    });
  }

  #[cfg(target_os = "macos")]
  mod os {
    use std::ffi::{c_char, c_void};

    #[link(name = "objc")]
    extern "C" {
      fn objc_getClass(name: *const c_char) -> *mut c_void;
      fn sel_registerName(name: *const c_char) -> *mut c_void;
      fn objc_msgSend();
    }

    /// `[NSApp terminate:nil]`, what Cmd+Q and the quit role send, from the
    /// run loop as a menu's action is. Main thread.
    pub fn terminate_soon() {
      type Get = unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void;
      type Perform = unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        *mut c_void,
        *mut c_void,
        f64,
      );
      unsafe {
        let get: Get =
          std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
        let perform: Perform =
          std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
        let app = get(
          objc_getClass(c"NSApplication".as_ptr()),
          sel_registerName(c"sharedApplication".as_ptr()),
        );
        perform(
          app,
          sel_registerName(c"performSelector:withObject:afterDelay:".as_ptr()),
          sel_registerName(c"terminate:".as_ptr()),
          std::ptr::null_mut(),
          0.0,
        );
      }
    }
  }

  #[cfg(target_os = "macos")]
  pub async fn run() {
    let w = Window::new(300, 200).title("native-e2e-quit-request");
    let _ = wait_for(|| w.get_size().0 != 0, 100, 50).await;
    install();

    let quit_from_ui = || {
      let _ = laufey::spawn_on_ui_thread(os::terminate_soon);
    };

    // A refusal keeps it running, and a later terminate: asks again.
    quit_from_ui();
    check(
      "terminate: asks the handler",
      wait_for(|| ASKED.load(Ordering::SeqCst) == 1, 100, 50).await,
    );
    check(
      "a terminate: outside a logout is no session end",
      SESSION_END.load(Ordering::SeqCst) == 0,
    );
    tokio::time::sleep(Duration::from_millis(800)).await;
    check("a refusal keeps the app running", !laufey::should_shutdown());
    check(
      "a window still answers after the refusal",
      w.get_size().0 != 0,
    );
    quit_from_ui();
    check(
      "the next terminate: asks again",
      wait_for(|| ASKED.load(Ordering::SeqCst) == 2, 100, 50).await,
    );
    tokio::time::sleep(Duration::from_millis(300)).await;

    // A quit Apple Event saying the user is logging out, as loginwindow sends:
    // a session end. Refused here, so it doesn't end the app.
    let before = ASKED.load(Ordering::SeqCst);
    let script = format!(
      "ObjC.import('Foundation');
       const target = $.NSAppleEventDescriptor
         .descriptorWithProcessIdentifier({});
       const ev = $.NSAppleEventDescriptor
         .appleEventWithEventClassEventIDTargetDescriptorReturnIDTransactionID(
           0x61657674, 0x71756974, target, -1, 0);
       ev.setAttributeDescriptorForKeyword(
         $.NSAppleEventDescriptor.descriptorWithEnumCode(0x6c6f676f),
         0x7768793f);
       ev.sendEventWithOptionsTimeoutError(0x1, 5, null);",
      std::process::id()
    );
    let sent = tokio::task::spawn_blocking(move || {
      std::process::Command::new("osascript")
        .args(["-l", "JavaScript", "-e", &script])
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .status()
        .is_ok_and(|s| s.success())
    })
    .await
    .unwrap_or(false);
    if wait_for(|| ASKED.load(Ordering::SeqCst) > before, 60, 50).await {
      check(
        "a logout's quit Apple Event asks as a session end",
        SESSION_END.load(Ordering::SeqCst) == 1,
      );
      check(
        "a refused logout keeps the app running",
        !laufey::should_shutdown(),
      );
    } else {
      na(&format!(
        "a logout's quit Apple Event (never arrived; osascript ok: {sent})"
      ));
    }
    tokio::time::sleep(Duration::from_millis(300)).await;

    // Silence: the deadline lets the quit go ahead. (`LAUFEY_E2E_QUIT_ALLOW=1`
    // takes the answering path instead; both end the process.)
    let allow = std::env::var("LAUFEY_E2E_QUIT_ALLOW").as_deref() == Ok("1");
    ANSWER.store(if allow { 1 } else { -1 }, Ordering::SeqCst);
    let started = Instant::now();
    quit_from_ui();
    let ended = wait_for(laufey::should_shutdown, 200, 50).await;
    check(
      &format!(
        "the app's end began after {:?} (deadline 5 s)",
        started.elapsed()
      ),
      ended && started.elapsed() < Duration::from_secs(7),
    );
    crate::finish();
  }

  #[cfg(windows)]
  mod os {
    use std::ffi::c_void;

    pub const WM_QUERYENDSESSION: u32 = 0x0011;
    pub const WM_ENDSESSION: u32 = 0x0016;
    pub const ENDSESSION_CRITICAL: isize = 0x4000_0000;

    #[link(name = "user32")]
    extern "system" {
      fn FindWindowExW(
        parent: *mut c_void,
        after: *mut c_void,
        class: *const u16,
        title: *const u16,
      ) -> *mut c_void;
      fn GetWindowThreadProcessId(hwnd: *mut c_void, pid: *mut u32) -> u32;
      fn SendMessageW(
        hwnd: *mut c_void,
        msg: u32,
        wp: usize,
        lp: isize,
      ) -> isize;
    }

    /// This process's hidden session-end window.
    pub fn watcher() -> Option<usize> {
      let class: Vec<u16> = "LaufeySessionEndWatcher"
        .encode_utf16()
        .chain(std::iter::once(0))
        .collect();
      let mut after = std::ptr::null_mut();
      loop {
        let hwnd = unsafe {
          FindWindowExW(
            std::ptr::null_mut(),
            after,
            class.as_ptr(),
            std::ptr::null(),
          )
        };
        if hwnd.is_null() {
          return None;
        }
        let mut pid = 0;
        unsafe { GetWindowThreadProcessId(hwnd, &mut pid) };
        if pid == std::process::id() {
          return Some(hwnd as usize);
        }
        after = hwnd;
      }
    }

    pub fn send(hwnd: usize, msg: u32, wp: usize, lp: isize) -> isize {
      unsafe { SendMessageW(hwnd as *mut c_void, msg, wp, lp) }
    }
  }

  #[cfg(windows)]
  async fn ask(hwnd: usize, lp: isize) -> (isize, Duration) {
    tokio::task::spawn_blocking(move || {
      let started = Instant::now();
      let r = os::send(hwnd, os::WM_QUERYENDSESSION, 0, lp);
      (r, started.elapsed())
    })
    .await
    .unwrap()
  }

  #[cfg(windows)]
  pub async fn run() {
    let w = Window::new(300, 200).title("native-e2e-quit-request");
    let _ = wait_for(|| w.get_size().0 != 0, 100, 50).await;
    install();

    let mut hwnd = None;
    let found = wait_for(
      || {
        hwnd = os::watcher();
        hwnd.is_some()
      },
      100,
      50,
    )
    .await;
    check("the hidden session-end window exists", found);
    let Some(hwnd) = hwnd else {
      crate::finish();
    };

    let (r, _) = ask(hwnd, 0).await;
    check(
      "WM_QUERYENDSESSION asks the handler, as a session end",
      ASKED.load(Ordering::SeqCst) == 1
        && SESSION_END.load(Ordering::SeqCst) == 1,
    );
    check("a refusal answers FALSE", r == 0);
    check("a refusal keeps the app running", !laufey::should_shutdown());

    ANSWER.store(1, Ordering::SeqCst);
    let (r, took) = ask(hwnd, 0).await;
    check("an answer that allows it answers TRUE", r != 0);
    check("... at once", took < Duration::from_secs(1));

    ANSWER.store(-1, Ordering::SeqCst);
    let (r, took) = ask(hwnd, 0).await;
    eprintln!("[e2e] INFO no answer: TRUE after {took:?}");
    check("a runtime that never answers can't hold it: TRUE", r != 0);
    check(
      "... after about the deadline, under Windows's 5 s",
      took >= Duration::from_millis(3500) && took < Duration::from_millis(4900),
    );

    let before = ASKED.load(Ordering::SeqCst);
    let (r, took) = ask(hwnd, os::ENDSESSION_CRITICAL).await;
    check(
      "a critical end asks nobody",
      r != 0
        && ASKED.load(Ordering::SeqCst) == before
        && took < Duration::from_secs(1),
    );

    check(
      "the app is still up: only WM_ENDSESSION ends it",
      !laufey::should_shutdown(),
    );
    os::send(hwnd, os::WM_ENDSESSION, 0, 0);
    tokio::time::sleep(Duration::from_millis(500)).await;
    check(
      "WM_ENDSESSION(FALSE) (another app refused) leaves the app up",
      !laufey::should_shutdown(),
    );
    let _ = tokio::task::spawn_blocking(move || {
      os::send(hwnd, os::WM_ENDSESSION, 1, 0);
    });
    check(
      "WM_ENDSESSION(TRUE) quits the app",
      wait_for(laufey::should_shutdown, 200, 50).await,
    );
    crate::finish();
  }
}
