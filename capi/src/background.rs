// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Background throttling (API 51): what the web engine does to a window's
//! page while the window is not on screen, chosen when the window is created
//! ([`crate::WindowOptions::background_throttling`]). See
//! `docs/window-management.md` ("Background throttling").

use crate::{
  api, LaufeyBackendApi, Window, LAUFEY_WINDOW_FLAG_BACKGROUND_SUSPEND,
  LAUFEY_WINDOW_FLAG_BACKGROUND_UNTHROTTLED,
};

pub const LAUFEY_BACKGROUND_THROTTLING_THROTTLE: i32 = 0;
pub const LAUFEY_BACKGROUND_THROTTLING_NONE: i32 = 1;
pub const LAUFEY_BACKGROUND_THROTTLING_SUSPEND: i32 = 2;

/// What a window's page does while the window is not on screen (created
/// hidden, hidden, minimized or covered).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum BackgroundThrottling {
  /// The page keeps running with slower timers (the default; Electron's
  /// `backgroundThrottling: true`).
  #[default]
  Throttle,
  /// The page runs as if it were on screen (Electron's
  /// `backgroundThrottling: false`).
  None,
  /// The engine may freeze the page until it is on screen again.
  Suspend,
}

impl BackgroundThrottling {
  /// The `LAUFEY_BACKGROUND_THROTTLING_*` value.
  pub fn to_raw(self) -> i32 {
    match self {
      BackgroundThrottling::Throttle => LAUFEY_BACKGROUND_THROTTLING_THROTTLE,
      BackgroundThrottling::None => LAUFEY_BACKGROUND_THROTTLING_NONE,
      BackgroundThrottling::Suspend => LAUFEY_BACKGROUND_THROTTLING_SUSPEND,
    }
  }

  /// The policy a `LAUFEY_BACKGROUND_THROTTLING_*` value names.
  pub fn from_raw(raw: i32) -> Option<Self> {
    match raw {
      LAUFEY_BACKGROUND_THROTTLING_THROTTLE => {
        Some(BackgroundThrottling::Throttle)
      }
      LAUFEY_BACKGROUND_THROTTLING_NONE => Some(BackgroundThrottling::None),
      LAUFEY_BACKGROUND_THROTTLING_SUSPEND => {
        Some(BackgroundThrottling::Suspend)
      }
      _ => None,
    }
  }

  /// The `LAUFEY_WINDOW_FLAG_BACKGROUND_*` creation flag (none for
  /// [`BackgroundThrottling::Throttle`]).
  pub fn to_flags(self) -> u32 {
    match self {
      BackgroundThrottling::Throttle => 0,
      BackgroundThrottling::None => LAUFEY_WINDOW_FLAG_BACKGROUND_UNTHROTTLED,
      BackgroundThrottling::Suspend => LAUFEY_WINDOW_FLAG_BACKGROUND_SUSPEND,
    }
  }
}

pub(crate) fn background_throttling_with(
  api: &LaufeyBackendApi,
  window_id: u32,
) -> Option<BackgroundThrottling> {
  let f = api.get_background_throttling?;
  BackgroundThrottling::from_raw(unsafe { f(api.backend_data, window_id) })
}

impl Window {
  /// The policy the window was created with (API 51); `None` where the
  /// backend doesn't choose one: CEF, WebView2 and WebKitGTK throttle a
  /// hidden page's timers without freezing it, and macOS before 14 never
  /// suspends a page.
  pub fn background_throttling(&self) -> Option<BackgroundThrottling> {
    background_throttling_with(api(), self.id())
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::WindowOptions;
  use std::ffi::c_void;

  unsafe extern "C" fn fake_get(_: *mut c_void, window: u32) -> i32 {
    match window {
      1 => LAUFEY_BACKGROUND_THROTTLING_NONE,
      2 => -1,
      _ => 7,
    }
  }

  #[test]
  fn policies_round_trip_their_raw_values() {
    for p in [
      BackgroundThrottling::Throttle,
      BackgroundThrottling::None,
      BackgroundThrottling::Suspend,
    ] {
      assert_eq!(BackgroundThrottling::from_raw(p.to_raw()), Some(p));
    }
    assert_eq!(BackgroundThrottling::from_raw(-1), None);
    assert_eq!(BackgroundThrottling::from_raw(3), None);
    assert_eq!(
      BackgroundThrottling::default(),
      BackgroundThrottling::Throttle
    );
  }

  #[test]
  fn the_policy_is_a_creation_flag() {
    let flags = |background_throttling| {
      WindowOptions {
        hidden: true,
        background_throttling,
        ..Default::default()
      }
      .to_flags()
    };
    let hidden = crate::LAUFEY_WINDOW_FLAG_HIDDEN;
    assert_eq!(flags(BackgroundThrottling::Throttle), hidden);
    assert_eq!(
      flags(BackgroundThrottling::None),
      hidden | LAUFEY_WINDOW_FLAG_BACKGROUND_UNTHROTTLED
    );
    assert_eq!(
      flags(BackgroundThrottling::Suspend),
      hidden | LAUFEY_WINDOW_FLAG_BACKGROUND_SUSPEND
    );
    // The default asks for nothing: a plain window still goes through
    // create_window.
    assert_eq!(WindowOptions::default().to_flags(), 0);
  }

  #[test]
  fn the_policy_reads_back_and_none_without_the_entry_point() {
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(background_throttling_with(&none, 1), None);
    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    api.get_background_throttling = Some(fake_get);
    assert_eq!(
      background_throttling_with(&api, 1),
      Some(BackgroundThrottling::None)
    );
    // -1 (no policy here) and values outside the enum read as none.
    assert_eq!(background_throttling_with(&api, 2), None);
    assert_eq!(background_throttling_with(&api, 3), None);
  }
}
