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
