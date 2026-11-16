// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! LAUFEY_E2E_ONLY=system | devtools-off (and the shortcut-holder helper):
//! global shortcuts, launch at login and DevTools control (API 40). See
//! docs/e2e-testing.md.
//!
//! - Global shortcuts: register / list / unregister round trip with the
//!   canonical form, ALREADY_REGISTERED for another spelling, INVALID for a
//!   bad or modifier-less accelerator, a press through the test hook (and,
//!   on Windows, a real key press through SendInput), and a CONFLICT the OS
//!   reports while a second process (this binary in shortcut-holder mode)
//!   holds the same combination, then OK once it let go.
//! - Launch at login: on -> state read back (plus the OS artefact: the HKCU
//!   Run value, the XDG autostart file) -> off -> read back. Only in CI or
//!   with LAUFEY_E2E_LOGIN_ITEM=1, so a developer's machine is left alone;
//!   the original state is restored.
//! - DevTools: enabled (launch setting and the engine's own setting read
//!   back), open / close / toggle round trips. devtools-off runs under
//!   LAUFEY_INSPECTABLE=0 and checks that everything stays closed.

use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{LoginItemState, ShortcutError, Window};

use super::{check, na, wait_for};

/// Unlikely to be bound by anything on a CI runner or a dev machine.
const SHORTCUT: &str = "Ctrl+Alt+Shift+F10";
const HELD_SHORTCUT: &str = "Ctrl+Alt+Shift+F11";

pub async fn run() {
  let caps = laufey::system_capabilities();
  eprintln!(
    "[e2e] system capabilities = {:#x} (shortcuts={} user_binds={} login={} devtools={})",
    caps.bits,
    caps.global_shortcuts(),
    caps.shortcuts_user_binds(),
    caps.launch_at_login(),
    caps.devtools()
  );
  shortcut_checks(&caps).await;
  login_item_checks(&caps).await;
  // LAUFEY_E2E_DEVTOOLS_REPEAT=<n>: the DevTools checks n times, each round
  // on a new window (and web view), counted at the end.
  let rounds = std::env::var("LAUFEY_E2E_DEVTOOLS_REPEAT")
    .ok()
    .and_then(|v| v.parse::<u32>().ok())
    .unwrap_or(1)
    .max(1);
  // Open through every round: closing a round's last window would quit the
  // app, and it makes two windows share the browser process (WebView2), so
  // a DevTools window is attributed by what open_devtools brought up, not by
  // being the process's only one. On Windows under
  // scripts/native-e2e-run.sh --system its controller creation fails twice
  // first (LAUFEY_TEST_WV2_CONTROLLER_FAILURES): it only gets a web view
  // through WebView2's documented retry from a new environment.
  let anchor = Window::new(320, 240).title("native-e2e-devtools-anchor");
  anchor.navigate("about:blank");
  anchor.show();
  if caps.devtools() {
    check(
      "the first window gets its web view (on Windows after two failed \
       controller creations)",
      wait_for(|| anchor.is_devtools_enabled(), 300, 50).await,
    );
  }
  let mut clean = 0;
  for i in 1..=rounds {
    let label = if rounds > 1 {
      format!(" (round {i}/{rounds})")
    } else {
      String::new()
    };
    if devtools_checks(&caps, true, &label).await {
      clean += 1;
    }
  }
  if rounds > 1 {
    eprintln!("[e2e] DevTools rounds passed: {clean}/{rounds}");
  }
  if cfg!(windows) && caps.devtools() {
    two_view_devtools_checks().await;
  }
  anchor.close();
}

/// WebView2: two windows of one browser process open DevTools at once. Their
/// DevTools windows are told apart only as the one that appeared after each
/// open, so the backend runs one open per process at a time: both open, and
/// each view's close and open state stay its own.
async fn two_view_devtools_checks() {
  let a = Window::new(400, 300).title("native-e2e-devtools-a");
  a.navigate("about:blank");
  a.show();
  let b = Window::new(400, 300).title("native-e2e-devtools-b");
  b.navigate("about:blank");
  b.show();
  wait_for(
    || a.is_devtools_enabled() && b.is_devtools_enabled(),
    300,
    50,
  )
  .await;
  a.open_devtools();
  b.open_devtools();
  check(
    "two windows opening DevTools at once both get theirs",
    wait_for(|| a.is_devtools_open() && b.is_devtools_open(), 300, 50).await,
  );
  a.close_devtools();
  let a_closed = wait_for(|| !a.is_devtools_open(), 300, 50).await;
  tokio::time::sleep(Duration::from_millis(1000)).await;
  check(
    "closing one window's DevTools leaves the other's open",
    a_closed && b.is_devtools_open(),
  );
  b.close_devtools();
  check(
    "the other window's DevTools close too",
    wait_for(|| !b.is_devtools_open(), 300, 50).await,
  );
  a.close();
  b.close();
}

pub async fn devtools_off() {
  let caps = laufey::system_capabilities();
  // Open through both checks: devtools_checks closes its window, and closing
  // the app's last window quits it, so the web view check's window would
  // never get its page (create_webview answered NoWindow while the app shut
  // down).
  let anchor = Window::new(320, 240).title("native-e2e-devtools-off-anchor");
  anchor.navigate("about:blank");
  anchor.show();
  devtools_checks(&caps, false, "").await;
  // A child web view's open_devtools follows the same switch (API 58).
  super::webview_checks::devtools_off().await;
  anchor.close();
}

// ---------------------------------------------------------------------------
// Global shortcuts
// ---------------------------------------------------------------------------

async fn shortcut_checks(caps: &laufey::SystemCapabilities) {
  if !caps.global_shortcuts() {
    // Winit, or Linux with neither X11 nor the portal.
    na("global shortcuts (not supported by this backend / session)");
    check(
      "register_shortcut is NOT_SUPPORTED without the capability",
      laufey::register_shortcut(SHORTCUT).await
        == Err(ShortcutError::NotSupported),
    );
    return;
  }
  if caps.shortcuts_user_binds() {
    // The portal asks the user; nothing can answer its dialog in CI.
    na("global shortcuts (the portal needs a user to approve them)");
    return;
  }

  let pressed: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
  {
    let pressed = pressed.clone();
    laufey::on_shortcut(move |accel| {
      pressed.lock().unwrap().push(accel.to_string());
    });
  }

  let canonical = laufey::register_shortcut(SHORTCUT).await;
  eprintln!("[e2e] register_shortcut({SHORTCUT}) = {canonical:?}");
  check(
    "register_shortcut answers OK with the canonical form",
    canonical.as_deref() == Ok(SHORTCUT),
  );
  check(
    "canonical_accelerator maps another spelling",
    laufey::canonical_accelerator("shift+alt+control+f10").as_deref()
      == Some(SHORTCUT),
  );
  check(
    "canonical_accelerator refuses garbage",
    laufey::canonical_accelerator("Ctrl+Nope").is_none(),
  );
  check(
    "shortcuts() lists it",
    laufey::shortcuts() == vec![SHORTCUT.to_string()],
  );
  check(
    "another spelling is ALREADY_REGISTERED",
    laufey::register_shortcut("shift+alt+control+f10").await
      == Err(ShortcutError::AlreadyRegistered),
  );
  check(
    "a printable key without a modifier is INVALID",
    laufey::register_shortcut("K").await == Err(ShortcutError::Invalid),
  );
  check(
    "a navigation key without a modifier is INVALID",
    laufey::register_shortcut("Escape").await == Err(ShortcutError::Invalid)
      && laufey::register_shortcut("Shift+Up").await
        == Err(ShortcutError::Invalid),
  );
  check(
    "an unknown key is INVALID",
    laufey::register_shortcut("Ctrl+Nope").await == Err(ShortcutError::Invalid),
  );

  // A press through the backend's own dispatch.
  check(
    "test_trigger_shortcut reaches the handler",
    laufey::test_trigger_shortcut("Shift+Alt+Ctrl+F10"),
  );
  check(
    "the handler gets the canonical accelerator",
    wait_for(
      || pressed.lock().unwrap().iter().any(|a| a == SHORTCUT),
      50,
      20,
    )
    .await,
  );

  real_press_check(&pressed).await;
  conflict_checks().await;

  check(
    "unregister_shortcut releases it",
    laufey::unregister_shortcut(SHORTCUT),
  );
  check("shortcuts() is empty again", laufey::shortcuts().is_empty());
  check(
    "a second unregister reports false",
    !laufey::unregister_shortcut(SHORTCUT),
  );
  check(
    "test_trigger_shortcut refuses an unregistered shortcut",
    !laufey::test_trigger_shortcut(SHORTCUT),
  );
  // And it can be taken again (the OS binding was really released).
  check(
    "re-registering after unregister is OK",
    laufey::register_shortcut(SHORTCUT).await.is_ok(),
  );
  laufey::unregister_all_shortcuts();
  check(
    "unregister_all_shortcuts empties the list",
    laufey::shortcuts().is_empty(),
  );
  laufey::clear_shortcut_handler();
}

/// A real key press reaches the global shortcut: on Windows injected with
/// SendInput and arriving as WM_HOTKEY on the UI thread; on X11 injected
/// with xdotool (XTEST), which the X server matches against the shortcut's
/// passive key grab. (macOS needs Accessibility permission to post key
/// events, so it relies on the test hook above.)
async fn real_press_check(pressed: &Arc<Mutex<Vec<String>>>) {
  let before = pressed.lock().unwrap().len();
  #[cfg(windows)]
  let watch: Option<win_input::LlWatch>;
  #[cfg(windows)]
  {
    // Injected input reaches a hotkey only on the session's input desktop;
    // on a locked or disconnected session it is dropped although SendInput
    // reports it sent.
    eprintln!("[e2e] real key press: {}", win_input::input_state());
    if let Some(reason) = win_input::input_unreachable_reason()
      .or_else(win_input::foreground_blocks_injection)
    {
      na(&format!("a real key press ({reason})"));
      return;
    }
    // Our own low-level keyboard hook watches for the injected F10 press:
    // seen means the input entered the session's input stream (and a miss
    // is lost after the hooks, or in the hotkey path); unseen means it never
    // entered it.
    watch = win_input::LlWatch::start();
    let sent = win_input::press_ctrl_alt_shift_f10();
    check("SendInput injected the key press", sent);
  }
  #[cfg(not(windows))]
  {
    if crate::os_view::xdotool().is_none() {
      na("a real key press (needs OS input injection: SendInput on Windows, xdotool on X11)");
      return;
    }
    check(
      "xdotool injected Ctrl+Alt+Shift+F10",
      crate::os_view::xdo(&["key", "--clearmodifiers", "ctrl+alt+shift+F10"])
        .is_some(),
    );
  }
  let fired =
    wait_for(|| pressed.lock().unwrap().len() > before, 100, 20).await;
  #[cfg(windows)]
  {
    match watch.map(win_input::LlWatch::finish) {
      Some(true) => eprintln!(
        "[e2e] real key press: our low-level keyboard hook saw the injected \
         F10 press (it entered the input stream)"
      ),
      Some(false) => eprintln!(
        "[e2e] real key press: our low-level keyboard hook did NOT see the \
         injected F10 press (it never entered the input stream)"
      ),
      None => eprintln!(
        "[e2e] real key press: no low-level keyboard hook could be installed"
      ),
    }
    if !fired {
      eprintln!(
        "[e2e] real key press: no hotkey within 2 s; now {}",
        win_input::input_state()
      );
    }
  }
  check("a real key press fires the global shortcut", fired);
  check(
    "the real press carries the canonical accelerator",
    pressed.lock().unwrap()[before..]
      .iter()
      .all(|a| a == SHORTCUT),
  );
}

/// A second process holds HELD_SHORTCUT: this one gets CONFLICT from the
/// OS, then OK once the holder has let go.
async fn conflict_checks() {
  let dir = std::env::temp_dir()
    .join(format!("laufey-e2e-shortcut-holder-{}", std::process::id()));
  let _ = std::fs::remove_dir_all(&dir);
  if std::fs::create_dir_all(&dir).is_err() {
    check("shortcut holder: temp dir", false);
    return;
  }
  let exe = match std::env::current_exe() {
    Ok(e) => e,
    Err(_) => {
      check("shortcut holder: current_exe", false);
      return;
    }
  };
  let mut cmd = std::process::Command::new(&exe);
  cmd
    .env("LAUFEY_E2E_ONLY", "shortcut-holder")
    .env("LAUFEY_E2E_HOLDER_DIR", &dir)
    // Its own web profile (CEF locks a profile directory per process).
    .env("LAUFEY_DATA_DIR", dir.join("data"))
    .env_remove("LAUFEY_SINGLE_INSTANCE");
  for arg in std::env::args().skip(1) {
    cmd.arg(arg);
  }
  let mut child = match cmd.spawn() {
    Ok(c) => c,
    Err(e) => {
      eprintln!("[e2e] spawning the shortcut holder failed: {e}");
      check("shortcut holder starts", false);
      return;
    }
  };
  let ready = dir.join("ready");
  let got_ready = wait_for(|| ready.exists(), 600, 50).await;
  let holder_status = std::fs::read_to_string(&ready).unwrap_or_default();
  eprintln!("[e2e] shortcut holder: {}", holder_status.trim());
  check(
    "the second process holds the shortcut",
    got_ready && holder_status.trim() == "ok",
  );
  if got_ready && holder_status.trim() == "ok" {
    let r = laufey::register_shortcut(HELD_SHORTCUT).await;
    eprintln!("[e2e] register_shortcut({HELD_SHORTCUT}) while held = {r:?}");
    check(
      "the OS refuses a shortcut another process holds (CONFLICT)",
      r == Err(ShortcutError::Conflict),
    );
    check(
      "a refused shortcut isn't listed",
      !laufey::shortcuts().iter().any(|s| s == HELD_SHORTCUT),
    );
  }
  let _ = std::fs::write(dir.join("release"), b"1");
  let mut exited = false;
  for _ in 0..600 {
    if matches!(child.try_wait(), Ok(Some(_))) {
      exited = true;
      break;
    }
    tokio::time::sleep(Duration::from_millis(50)).await;
  }
  if !exited {
    let _ = child.kill();
  }
  check("the shortcut holder exits", exited);
  if got_ready && holder_status.trim() == "ok" {
    let r = laufey::register_shortcut(HELD_SHORTCUT).await;
    check(
      "the shortcut is free once the other process let go",
      r.as_deref() == Ok(HELD_SHORTCUT),
    );
    laufey::unregister_shortcut(HELD_SHORTCUT);
  }
  let _ = child.wait();
  let _ = std::fs::remove_dir_all(&dir);
}

/// The shortcut-holder process: registers HELD_SHORTCUT, reports, and holds
/// it until told to let go (or a minute passes).
pub async fn hold_shortcut() {
  let Some(dir) = std::env::var_os("LAUFEY_E2E_HOLDER_DIR").map(PathBuf::from)
  else {
    eprintln!("[e2e-holder] LAUFEY_E2E_HOLDER_DIR is not set");
    return;
  };
  let r = laufey::register_shortcut(HELD_SHORTCUT).await;
  let status = match &r {
    Ok(_) => "ok".to_string(),
    Err(e) => e.code().to_string(),
  };
  let tmp = dir.join("ready.tmp");
  let _ = std::fs::write(&tmp, status.as_bytes());
  let _ = std::fs::rename(&tmp, dir.join("ready"));
  let release = dir.join("release");
  wait_for(|| release.exists(), 1200, 50).await;
  laufey::unregister_all_shortcuts();
  // Let the UI thread release the OS binding before the process ends.
  tokio::time::sleep(Duration::from_millis(200)).await;
}

#[cfg(windows)]
mod win_input {
  use std::ffi::c_void;
  #[repr(C)]
  #[derive(Clone, Copy)]
  struct KeybdInput {
    vk: u16,
    scan: u16,
    flags: u32,
    time: u32,
    extra: usize,
  }

  // INPUT: the type, then a union whose largest member (MOUSEINPUT) is 8
  // bytes bigger than KEYBDINPUT.
  #[repr(C)]
  #[derive(Clone, Copy)]
  struct Input {
    kind: u32,
    ki: KeybdInput,
    _pad: [u8; 8],
  }

  extern "system" {
    fn SendInput(count: u32, inputs: *const Input, size: i32) -> u32;
    fn OpenInputDesktop(flags: u32, inherit: i32, access: u32) -> isize;
    fn CloseDesktop(desktop: isize) -> i32;
    fn GetThreadDesktop(thread: u32) -> isize;
    fn GetCurrentThreadId() -> u32;
    fn GetUserObjectInformationW(
      object: isize,
      index: i32,
      info: *mut u16,
      len: u32,
      needed: *mut u32,
    ) -> i32;
    fn GetLastError() -> u32;
    fn SetLastError(code: u32);
    fn GetForegroundWindow() -> *mut c_void;
    fn GetWindowThreadProcessId(hwnd: *mut c_void, pid: *mut u32) -> u32;
    fn GetClassNameW(hwnd: *mut c_void, buf: *mut u16, len: i32) -> i32;
    fn GetWindowTextW(hwnd: *mut c_void, buf: *mut u16, len: i32) -> i32;
    fn GetCurrentProcessId() -> u32;
    fn ProcessIdToSessionId(pid: u32, session: *mut u32) -> i32;
    fn WTSGetActiveConsoleSessionId() -> u32;
  }

  fn utf16_text(buf: &[u16], len: i32) -> String {
    let len = (len.max(0) as usize).min(buf.len());
    String::from_utf16_lossy(&buf[..len])
  }

  #[link(name = "advapi32")]
  extern "system" {
    fn OpenProcessToken(
      process: *mut c_void,
      access: u32,
      token: *mut *mut c_void,
    ) -> i32;
    fn GetTokenInformation(
      token: *mut c_void,
      class: i32,
      info: *mut c_void,
      len: u32,
      ret_len: *mut u32,
    ) -> i32;
    fn GetSidSubAuthorityCount(sid: *mut c_void) -> *mut u8;
    fn GetSidSubAuthority(sid: *mut c_void, index: u32) -> *mut u32;
  }

  extern "system" {
    fn OpenProcess(access: u32, inherit: i32, pid: u32) -> *mut c_void;
    fn GetCurrentProcess() -> *mut c_void;
    fn CloseHandle(h: *mut c_void) -> i32;
  }

  const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x1000;
  const TOKEN_QUERY: u32 = 0x0008;
  const TOKEN_INTEGRITY_LEVEL: i32 = 25;

  /// The mandatory integrity level (a RID: 0x2000 medium, 0x3000 high,
  /// 0x4000 system) of `process`'s token, or None if it can't be read.
  fn integrity_of(process: *mut c_void) -> Option<u32> {
    let mut token: *mut c_void = std::ptr::null_mut();
    if unsafe { OpenProcessToken(process, TOKEN_QUERY, &mut token) } == 0 {
      return None;
    }
    let mut buf = [0u8; 128];
    let mut needed = 0u32;
    let ok = unsafe {
      GetTokenInformation(
        token,
        TOKEN_INTEGRITY_LEVEL,
        buf.as_mut_ptr() as *mut c_void,
        buf.len() as u32,
        &mut needed,
      )
    };
    unsafe { CloseHandle(token) };
    if ok == 0 {
      return None;
    }
    // TOKEN_MANDATORY_LABEL: SID_AND_ATTRIBUTES { PSID Sid; DWORD Attributes }.
    let sid = unsafe { *(buf.as_ptr() as *const *mut c_void) };
    if sid.is_null() {
      return None;
    }
    let count = unsafe { *GetSidSubAuthorityCount(sid) } as u32;
    if count == 0 {
      return None;
    }
    Some(unsafe { *GetSidSubAuthority(sid, count - 1) })
  }

  /// Why injected input can't reach the hotkey because of the foreground
  /// window, if so: another process's secure dialog (the Windows Security
  /// credential dialog) or a window of a process at a higher integrity level
  /// than this one holds the foreground, and the system (UIPI, secure UI)
  /// drops the injected keys there.
  pub fn foreground_blocks_injection() -> Option<String> {
    let fg = unsafe { GetForegroundWindow() };
    if fg.is_null() {
      return None;
    }
    let mut pid = 0u32;
    unsafe { GetWindowThreadProcessId(fg, &mut pid) };
    if pid == 0 || pid == unsafe { GetCurrentProcessId() } {
      return None;
    }
    let mut class = [0u16; 128];
    let n =
      unsafe { GetClassNameW(fg, class.as_mut_ptr(), class.len() as i32) };
    let class = utf16_text(&class, n);
    if class == "Credential Dialog Xaml Host" {
      return Some(format!(
        "the foreground is another process's Windows Security credential \
         dialog (class {class:?}, pid {pid}), which takes no injected input"
      ));
    }
    let ours = integrity_of(unsafe { GetCurrentProcess() })?;
    let handle =
      unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
    if handle.is_null() {
      return None;
    }
    let theirs = integrity_of(handle);
    unsafe { CloseHandle(handle) };
    match theirs {
      Some(level) if level > ours => Some(format!(
        "the foreground window (class {class:?}, pid {pid}) runs at integrity \
         level {level:#x}, above this process's {ours:#x}, so UIPI drops \
         injected input"
      )),
      _ => None,
    }
  }

  /// What injected input meets on this machine, for the log of the real key
  /// press: the input desktop against this thread's, the foreground window
  /// and whose it is, and this process's session against the console's.
  pub fn input_state() -> String {
    let input = unsafe { OpenInputDesktop(0, 0, DESKTOP_READOBJECTS) };
    let input_desc = if input == 0 {
      format!("OpenInputDesktop failed (error {})", unsafe {
        GetLastError()
      })
    } else {
      let name = desktop_name(input);
      unsafe { CloseDesktop(input) };
      format!("input desktop {name:?}")
    };
    let ours = desktop_name(unsafe { GetThreadDesktop(GetCurrentThreadId()) });
    let fg = unsafe { GetForegroundWindow() };
    let mut fg_pid = 0u32;
    let mut class = [0u16; 128];
    let mut title = [0u16; 128];
    let (class_len, title_len) = if !fg.is_null() {
      unsafe {
        GetWindowThreadProcessId(fg, &mut fg_pid);
        (
          GetClassNameW(fg, class.as_mut_ptr(), class.len() as i32),
          GetWindowTextW(fg, title.as_mut_ptr(), title.len() as i32),
        )
      }
    } else {
      (0, 0)
    };
    let pid = unsafe { GetCurrentProcessId() };
    let mut session = u32::MAX;
    unsafe { ProcessIdToSessionId(pid, &mut session) };
    let console = unsafe { WTSGetActiveConsoleSessionId() };
    format!(
      "{input_desc}, this thread's desktop {ours:?}; foreground window \
       {fg:?} class {:?} title {:?} pid {fg_pid} ({}); session {session}, \
       console session {console}",
      utf16_text(&class, class_len),
      utf16_text(&title, title_len),
      if fg.is_null() {
        "none"
      } else if fg_pid == pid {
        "this process"
      } else {
        "another process"
      },
    )
  }

  const UOI_NAME: i32 = 2;
  const DESKTOP_READOBJECTS: u32 = 0x0001;

  fn desktop_name(desktop: isize) -> Option<String> {
    let mut buf = [0u16; 256];
    let mut needed = 0u32;
    let ok = unsafe {
      GetUserObjectInformationW(
        desktop,
        UOI_NAME,
        buf.as_mut_ptr(),
        (buf.len() * 2) as u32,
        &mut needed,
      )
    };
    if ok == 0 {
      return None;
    }
    let len = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    Some(String::from_utf16_lossy(&buf[..len]))
  }

  /// Why injected input can't reach this process's desktop, if it can't:
  /// SendInput reports the events as sent either way, but the system drops
  /// them when this session has no input desktop (disconnected) or the input
  /// desktop is another one (the workstation is locked: Winlogon's secure
  /// desktop; a UAC prompt; the screen saver), and a hotkey only fires for
  /// input on the input desktop.
  pub fn input_unreachable_reason() -> Option<String> {
    let input = unsafe { OpenInputDesktop(0, 0, DESKTOP_READOBJECTS) };
    if input == 0 {
      let err = unsafe { GetLastError() };
      return Some(format!(
        "this session has no input desktop it can open (OpenInputDesktop \
         error {err}: locked, disconnected or on a secure desktop)"
      ));
    }
    let input_name = desktop_name(input);
    unsafe { CloseDesktop(input) };
    let ours = desktop_name(unsafe { GetThreadDesktop(GetCurrentThreadId()) });
    match (input_name, ours) {
      (Some(i), Some(o)) if !i.eq_ignore_ascii_case(&o) => Some(format!(
        "input goes to desktop {i:?}, not this process's {o:?}"
      )),
      _ => None,
    }
  }

  const INPUT_KEYBOARD: u32 = 1;
  const KEYEVENTF_KEYUP: u32 = 2;

  fn key(vk: u16, up: bool) -> Input {
    Input {
      kind: INPUT_KEYBOARD,
      ki: KeybdInput {
        vk,
        scan: 0,
        flags: if up { KEYEVENTF_KEYUP } else { 0 },
        time: 0,
        extra: 0,
      },
      _pad: [0; 8],
    }
  }

  extern "system" {
    fn SetWindowsHookExW(
      id: i32,
      proc_: extern "system" fn(i32, usize, isize) -> isize,
      module: *mut c_void,
      thread: u32,
    ) -> *mut c_void;
    fn UnhookWindowsHookEx(hook: *mut c_void) -> i32;
    fn CallNextHookEx(
      hook: *mut c_void,
      code: i32,
      wparam: usize,
      lparam: isize,
    ) -> isize;
    fn GetModuleHandleW(name: *const u16) -> *mut c_void;
    fn PeekMessageW(
      msg: *mut [u64; 8],
      hwnd: *mut c_void,
      min: u32,
      max: u32,
      remove: u32,
    ) -> i32;
  }

  #[repr(C)]
  struct KbdLlHookStruct {
    vk: u32,
    scan: u32,
    flags: u32,
    time: u32,
    extra: usize,
  }

  const WH_KEYBOARD_LL: i32 = 13;
  const WM_KEYDOWN: usize = 0x0100;
  const WM_SYSKEYDOWN: usize = 0x0104;
  const LLKHF_INJECTED: u32 = 0x10;
  const PM_REMOVE: u32 = 1;
  const VK_F10: u32 = 0x79;

  static INJECTED_F10_SEEN: std::sync::atomic::AtomicBool =
    std::sync::atomic::AtomicBool::new(false);

  extern "system" fn ll_keyboard(
    code: i32,
    wparam: usize,
    lparam: isize,
  ) -> isize {
    if code >= 0 && (wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN) {
      let k = unsafe { &*(lparam as *const KbdLlHookStruct) };
      if k.vk == VK_F10 && k.flags & LLKHF_INJECTED != 0 {
        INJECTED_F10_SEEN.store(true, std::sync::atomic::Ordering::SeqCst);
      }
    }
    unsafe { CallNextHookEx(std::ptr::null_mut(), code, wparam, lparam) }
  }

  /// A WH_KEYBOARD_LL hook on a thread of its own (a low-level hook is
  /// called through its thread's message loop), installed for one check and
  /// removed on every path: when finish() is called, or after 3 s at most.
  pub struct LlWatch {
    done: std::sync::mpsc::Sender<()>,
    thread: std::thread::JoinHandle<()>,
  }

  impl LlWatch {
    /// None when the hook can't be installed.
    pub fn start() -> Option<LlWatch> {
      INJECTED_F10_SEEN.store(false, std::sync::atomic::Ordering::SeqCst);
      let (ready_tx, ready_rx) = std::sync::mpsc::channel::<bool>();
      let (done, done_rx) = std::sync::mpsc::channel::<()>();
      let thread = std::thread::spawn(move || {
        let hook = unsafe {
          SetWindowsHookExW(
            WH_KEYBOARD_LL,
            ll_keyboard,
            GetModuleHandleW(std::ptr::null()),
            0,
          )
        };
        let _ = ready_tx.send(!hook.is_null());
        if hook.is_null() {
          return;
        }
        let end = std::time::Instant::now() + std::time::Duration::from_secs(3);
        let mut msg = [0u64; 8];
        while std::time::Instant::now() < end && done_rx.try_recv().is_err() {
          while unsafe {
            PeekMessageW(&mut msg, std::ptr::null_mut(), 0, 0, PM_REMOVE)
          } != 0
          {}
          std::thread::sleep(std::time::Duration::from_millis(5));
        }
        unsafe { UnhookWindowsHookEx(hook) };
      });
      match ready_rx.recv() {
        Ok(true) => Some(LlWatch { done, thread }),
        _ => {
          let _ = thread.join();
          None
        }
      }
    }

    /// Removes the hook and says whether it saw the injected F10 press.
    pub fn finish(self) -> bool {
      let _ = self.done.send(());
      let _ = self.thread.join();
      INJECTED_F10_SEEN.load(std::sync::atomic::Ordering::SeqCst)
    }
  }

  pub fn press_ctrl_alt_shift_f10() -> bool {
    const CTRL: u16 = 0x11;
    const ALT: u16 = 0x12;
    const SHIFT: u16 = 0x10;
    const F10: u16 = 0x79;
    let seq = [
      key(CTRL, false),
      key(ALT, false),
      key(SHIFT, false),
      key(F10, false),
      key(F10, true),
      key(SHIFT, true),
      key(ALT, true),
      key(CTRL, true),
    ];
    unsafe { SetLastError(0) };
    let sent = unsafe {
      SendInput(
        seq.len() as u32,
        seq.as_ptr(),
        std::mem::size_of::<Input>() as i32,
      )
    };
    let err = unsafe { GetLastError() };
    eprintln!(
      "[e2e] SendInput returned {sent} of {} (GetLastError {err})",
      seq.len()
    );
    sent as usize == seq.len()
  }
}

// ---------------------------------------------------------------------------
// Launch at login
// ---------------------------------------------------------------------------

async fn login_item_checks(caps: &laufey::SystemCapabilities) {
  if !caps.launch_at_login() {
    na("launch at login (not supported by this backend / OS)");
    check(
      "launch_at_login is NOT_SUPPORTED without the capability",
      laufey::launch_at_login() == LoginItemState::NotSupported,
    );
    return;
  }
  let allowed = std::env::var_os("CI").is_some()
    || std::env::var("LAUFEY_E2E_LOGIN_ITEM").as_deref() == Ok("1");
  if !allowed {
    na("launch at login round trip (set CI or LAUFEY_E2E_LOGIN_ITEM=1)");
    return;
  }
  let initial = laufey::launch_at_login();
  eprintln!("[e2e] launch_at_login (initial) = {initial:?}");

  let on = laufey::set_launch_at_login(true);
  eprintln!("[e2e] set_launch_at_login(true) = {on:?}");
  let on_ok = matches!(
    on,
    Ok(LoginItemState::Enabled) | Ok(LoginItemState::RequiresApproval)
  );
  check("set_launch_at_login(true) turns it on", on_ok);
  if let Ok(state) = on {
    check(
      "launch_at_login reads the new state back",
      laufey::launch_at_login() == state,
    );
  }
  check(
    "the OS has the login entry",
    login_artifact_present() != Some(false),
  );

  let off = laufey::set_launch_at_login(false);
  eprintln!("[e2e] set_launch_at_login(false) = {off:?}");
  check(
    "set_launch_at_login(false) turns it off",
    off == Ok(LoginItemState::Disabled),
  );
  check(
    "launch_at_login reads disabled back",
    laufey::launch_at_login() == LoginItemState::Disabled,
  );
  check(
    "the OS login entry is gone",
    login_artifact_present() != Some(true),
  );

  if matches!(
    initial,
    LoginItemState::Enabled | LoginItemState::RequiresApproval
  ) {
    let _ = laufey::set_launch_at_login(true);
  }
}

fn login_item_name() -> String {
  if let Ok(id) = std::env::var("LAUFEY_APP_ID") {
    if !id.is_empty() {
      return id;
    }
  }
  std::env::current_exe()
    .ok()
    .and_then(|p| p.file_stem().map(|s| s.to_string_lossy().into_owned()))
    .unwrap_or_default()
}

/// Whether the OS-level entry exists: the XDG autostart file on Linux, the
/// HKCU Run value on Windows. None where it can't be checked from outside
/// (macOS keeps SMAppService state in its own database).
fn login_artifact_present() -> Option<bool> {
  let name = login_item_name();
  if cfg!(target_os = "linux") {
    let base = std::env::var_os("XDG_CONFIG_HOME")
      .map(PathBuf::from)
      .filter(|p| p.is_absolute())
      .or_else(|| {
        std::env::var_os("HOME").map(|h| Path::new(&h).join(".config"))
      })?;
    let file = base.join("autostart").join(format!("{name}.desktop"));
    let present = file.exists();
    if present {
      let text = std::fs::read_to_string(&file).unwrap_or_default();
      let exe = std::env::current_exe().ok()?;
      eprintln!("[e2e] {}:\n{text}", file.display());
      return Some(text.contains(&*exe.to_string_lossy()));
    }
    return Some(false);
  }
  if cfg!(windows) {
    let out = std::process::Command::new("reg")
      .args([
        "query",
        r"HKCU\Software\Microsoft\Windows\CurrentVersion\Run",
        "/v",
        &name,
      ])
      .output()
      .ok()?;
    let text = String::from_utf8_lossy(&out.stdout).to_string();
    if !out.status.success() {
      return Some(false);
    }
    let exe = std::env::current_exe().ok()?;
    eprintln!("[e2e] HKCU Run value: {}", text.trim());
    return Some(
      text
        .to_lowercase()
        .contains(&exe.to_string_lossy().to_lowercase()),
    );
  }
  None
}

// ---------------------------------------------------------------------------
// DevTools
// ---------------------------------------------------------------------------

/// One round of the DevTools checks; true when every check in it passed.
/// `label` ends each check's name (the round, when repeated).
async fn devtools_checks(
  caps: &laufey::SystemCapabilities,
  expect_on: bool,
  label: &str,
) -> bool {
  let all_ok = std::cell::Cell::new(true);
  let check = |name: &str, ok: bool| {
    if !ok {
      all_ok.set(false);
    }
    check(&format!("{name}{label}"), ok);
  };
  if !caps.devtools() {
    na("DevTools (no web engine)");
    check("devtools_enabled is false without an engine", {
      !laufey::devtools_enabled()
    });
    return all_ok.get();
  }
  check(
    if expect_on {
      "DevTools are enabled by default"
    } else {
      "LAUFEY_INSPECTABLE=0 disables DevTools"
    },
    laufey::devtools_enabled() == expect_on,
  );

  if expect_on {
    // Asked for at once, before the web view exists (WebView2 and CEF
    // create it asynchronously): the open must wait for it, not be lost.
    let early = Window::new(480, 360).title("native-e2e-devtools-early");
    early.navigate("about:blank");
    early.show();
    early.open_devtools();
    let asked = std::time::Instant::now();
    let opened = wait_for(|| early.is_devtools_open(), 300, 50).await;
    eprintln!(
      "[e2e] open_devtools at creation: open={opened} after {} ms",
      asked.elapsed().as_millis()
    );
    check(
      "open_devtools right at window creation opens them once the web view \
       exists",
      opened,
    );
    check(
      "the engine's DevTools setting is on in that window",
      early.is_devtools_enabled(),
    );
    early.close_devtools();
    check(
      "close_devtools closes them",
      wait_for(|| !early.is_devtools_open(), 300, 50).await,
    );
    early.close();
  }

  let w = Window::new(480, 360).title("native-e2e-devtools");
  w.navigate("about:blank");
  w.show();
  if !wait_for(|| w.get_size().0 != 0, 100, 50).await {
    check("devtools window reports a size", false);
    return all_ok.get();
  }
  // The engine's own setting, read back (the web view may still be coming
  // up on WebView2).
  check(
    "the engine's DevTools setting matches the launch setting",
    wait_for(|| w.is_devtools_enabled() == expect_on, 100, 50).await,
  );
  check("DevTools start closed", !w.is_devtools_open());

  if expect_on {
    w.open_devtools();
    let asked = std::time::Instant::now();
    let opened = wait_for(|| w.is_devtools_open(), 300, 50).await;
    eprintln!(
      "[e2e] open_devtools: open={opened} after {} ms",
      asked.elapsed().as_millis()
    );
    check("open_devtools opens them", opened);
    w.close_devtools();
    check(
      "close_devtools closes them",
      wait_for(|| !w.is_devtools_open(), 300, 50).await,
    );
    // A navigation must not reset the engine's setting (WebView2 keeps
    // ICoreWebView2Settings per web view, across documents).
    w.navigate("about:blank");
    tokio::time::sleep(Duration::from_millis(1000)).await;
    check(
      "the engine's DevTools setting is still on after a navigation",
      w.is_devtools_enabled(),
    );
    w.toggle_devtools();
    let asked = std::time::Instant::now();
    let opened = wait_for(|| w.is_devtools_open(), 300, 50).await;
    eprintln!(
      "[e2e] toggle_devtools: open={opened} after {} ms",
      asked.elapsed().as_millis()
    );
    check("toggle_devtools opens them", opened);
    w.toggle_devtools();
    check(
      "toggle_devtools closes them again",
      wait_for(|| !w.is_devtools_open(), 300, 50).await,
    );
    // Closed while still opening: the engine may finish opening them after
    // the call returns (WebKitGTK shows the inspector once its web process
    // answers), and that must not bring them back.
    w.open_devtools();
    w.close_devtools();
    check(
      "DevTools closed right after open read as closed",
      !w.is_devtools_open(),
    );
    tokio::time::sleep(Duration::from_secs(3)).await;
    check(
      "DevTools closed right after open stay closed",
      !w.is_devtools_open(),
    );
    w.toggle_devtools();
    check(
      "toggle_devtools opens them after that",
      wait_for(|| w.is_devtools_open(), 300, 50).await,
    );
    w.close_devtools();
    check(
      "close_devtools closes them after that",
      wait_for(|| !w.is_devtools_open(), 300, 50).await,
    );
  } else {
    w.open_devtools();
    w.toggle_devtools();
    tokio::time::sleep(Duration::from_secs(3)).await;
    check(
      "open_devtools / toggle_devtools do nothing",
      !w.is_devtools_open(),
    );
    // CEF renders PDFs through the DevTools protocol (Page.printToPDF);
    // turning the DevTools UI off must not break it. (The other engines'
    // PDF paths don't involve DevTools; the main battery covers them.)
    if std::env::var("LAUFEY_E2E_BACKEND").as_deref() != Ok("cef") {
      na("print_to_pdf with DevTools off (only CEF prints through DevTools)");
      w.close();
      return all_ok.get();
    }
    let mut outcome = None;
    for attempt in 0..3u32 {
      if attempt > 0 {
        tokio::time::sleep(Duration::from_millis(700)).await;
      }
      let slot = Arc::new(Mutex::new(None::<Result<Vec<u8>, String>>));
      let s2 = slot.clone();
      w.print_to_pdf(None, move |r| *s2.lock().unwrap() = Some(r));
      if !wait_for(|| slot.lock().unwrap().is_some(), 150, 100).await {
        continue;
      }
      let r = slot.lock().unwrap().take().unwrap();
      let transient = matches!(&r, Err(e) if !e.contains("not supported"));
      outcome = Some(r);
      if !transient {
        break;
      }
    }
    match outcome {
      Some(Ok(bytes)) => check(
        "print_to_pdf still works with DevTools off",
        bytes.starts_with(b"%PDF-"),
      ),
      Some(Err(e)) if e.contains("not supported") => {
        na("print_to_pdf with DevTools off (no PDF support)")
      }
      other => {
        eprintln!("[e2e] print_to_pdf with DevTools off: {other:?}");
        check("print_to_pdf still works with DevTools off", false);
      }
    }
  }
  w.close();
  all_ok.get()
}
