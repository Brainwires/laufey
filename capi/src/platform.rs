// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Platform features (API 45): what this session provides, probed by the
//! backend instead of guessed from the desktop's name. See
//! `docs/platform-features.md` for the keys. May be called from any thread.

use crate::io::take_backend_string;
use crate::{api, LaufeyBackendApi};

/// The backend's platform-features JSON object, or `None` when the backend
/// can't say (older than API 45, or an allocation failure).
///
/// On Linux it says whether a tray icon can be seen (`"trayHost"`, and
/// `"trayReason"` when not), what the Secret Service can do without a
/// prompt, the session type and the xdg-desktop-portal versions; CEF adds the
/// cookie store it chose (`"cookieEncryption"`).
pub fn platform_features() -> Option<String> {
  platform_features_with(api())
}

fn platform_features_with(api: &LaufeyBackendApi) -> Option<String> {
  let f = api.platform_features?;
  unsafe { take_backend_string(api, f(api.backend_data)) }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::ffi::{c_char, c_void, CString};

  unsafe extern "C" fn fake_features(_: *mut c_void) -> *mut c_char {
    CString::new("{\"os\":\"linux\",\"trayHost\":false}")
      .unwrap()
      .into_raw()
  }
  unsafe extern "C" fn fake_string_free(_: *mut c_void, s: *mut c_char) {
    drop(unsafe { CString::from_raw(s) });
  }

  #[test]
  fn features() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(platform_features_with(&fake), None);
    fake.platform_features = Some(fake_features);
    fake.string_free = Some(fake_string_free);
    assert_eq!(
      platform_features_with(&fake).as_deref(),
      Some("{\"os\":\"linux\",\"trayHost\":false}")
    );
  }
}
