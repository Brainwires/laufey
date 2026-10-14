// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The navigation guard (API 48): a window's top-level frame loads only the
//! origin the embedder navigated it to, the origins allowed here, and
//! about:blank. Any other top-level navigation is cancelled, and an http(s)
//! destination opens in the OS browser. Sub-frames are left to the page's
//! Content Security Policy. See `docs/window-management.md`, "External links
//! and popups".

use std::ffi::{c_char, CString};

use crate::io::take_backend_string;
use crate::{api, LaufeyBackendApi};

/// Set the origins every window's top-level frame may load besides the one
/// the embedder navigated it to (exact origins, `scheme://host[:port]`),
/// replacing any set before. The backend reports and skips an entry that is
/// not an origin; an entry with a NUL byte is skipped here. Returns `false`
/// when the backend has no guard (no web engine, or older than API 48).
pub fn set_allowed_navigation_origins<S: AsRef<str>>(origins: &[S]) -> bool {
  set_allowed_navigation_origins_with(api(), origins)
}

fn set_allowed_navigation_origins_with<S: AsRef<str>>(
  api: &LaufeyBackendApi,
  origins: &[S],
) -> bool {
  let Some(f) = api.set_allowed_navigation_origins else {
    return false;
  };
  let owned: Vec<CString> = origins
    .iter()
    .filter_map(|o| CString::new(o.as_ref()).ok())
    .collect();
  let ptrs: Vec<*const c_char> = owned.iter().map(|c| c.as_ptr()).collect();
  unsafe { f(api.backend_data, ptrs.as_ptr(), ptrs.len()) };
  true
}

/// Test hook: while on, every URL the backend would open in the OS browser
/// (the navigation guard, a popup, the reserved open-external call) is
/// recorded instead (see [`test_take_external_opens`]). Turning it off drops
/// the record. Returns `false` when the backend has no such hook.
pub fn test_capture_external_opens(capture: bool) -> bool {
  let api = api();
  match api.test_capture_external_opens {
    Some(f) => {
      unsafe { f(api.backend_data, capture) };
      true
    }
    None => false,
  }
}

/// Test hook: the URLs recorded since the last call while capture was on,
/// oldest first; `None` when the backend has no such hook.
pub fn test_take_external_opens() -> Option<Vec<String>> {
  test_take_external_opens_with(api())
}

fn test_take_external_opens_with(
  api: &LaufeyBackendApi,
) -> Option<Vec<String>> {
  let f = api.test_take_external_opens?;
  let json = unsafe { take_backend_string(api, f(api.backend_data)) }?;
  parse_string_array(&json)
}

/// Parses the backend's JSON array of strings (`["a","b\"c"]`).
fn parse_string_array(json: &str) -> Option<Vec<String>> {
  let mut chars = json.trim().chars().peekable();
  if chars.next()? != '[' {
    return None;
  }
  let mut out = Vec::new();
  loop {
    match chars.next()? {
      ']' if out.is_empty() => return Some(out),
      '"' => {}
      _ => return None,
    }
    let mut s = String::new();
    loop {
      match chars.next()? {
        '"' => break,
        '\\' => match chars.next()? {
          '"' => s.push('"'),
          '\\' => s.push('\\'),
          '/' => s.push('/'),
          'n' => s.push('\n'),
          't' => s.push('\t'),
          'r' => s.push('\r'),
          'u' => {
            let hex: String = (0..4).filter_map(|_| chars.next()).collect();
            let code = u32::from_str_radix(&hex, 16).ok()?;
            s.push(char::from_u32(code).unwrap_or('\u{fffd}'));
          }
          _ => return None,
        },
        c => s.push(c),
      }
    }
    out.push(s);
    match chars.next()? {
      ',' => continue,
      ']' => return Some(out),
      _ => return None,
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::ffi::{c_void, CStr};
  use std::sync::Mutex;

  static SEEN: Mutex<Vec<String>> = Mutex::new(Vec::new());

  unsafe extern "C" fn fake_set(
    _: *mut c_void,
    origins: *const *const c_char,
    count: usize,
  ) {
    let mut seen = SEEN.lock().unwrap();
    seen.clear();
    for i in 0..count {
      let p = unsafe { *origins.add(i) };
      seen.push(unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned());
    }
  }

  unsafe extern "C" fn fake_take(_: *mut c_void) -> *mut c_char {
    CString::new(r#"["https://a.example/","b\"c\\d","e\u000af"]"#)
      .unwrap()
      .into_raw()
  }

  unsafe extern "C" fn fake_string_free(_: *mut c_void, s: *mut c_char) {
    drop(unsafe { CString::from_raw(s) });
  }

  #[test]
  fn not_supported_without_the_entry_points() {
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert!(!set_allowed_navigation_origins_with(&none, &["https://a"]));
    assert_eq!(test_take_external_opens_with(&none), None);
  }

  #[test]
  fn hands_the_origins_over() {
    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    api.set_allowed_navigation_origins = Some(fake_set);
    assert!(set_allowed_navigation_origins_with(
      &api,
      &["https://auth.example.com", "bad\0entry", "app://localhost"]
    ));
    assert_eq!(
      *SEEN.lock().unwrap(),
      vec!["https://auth.example.com", "app://localhost"]
    );
    let empty: [&str; 0] = [];
    assert!(set_allowed_navigation_origins_with(&api, &empty));
    assert!(SEEN.lock().unwrap().is_empty());
  }

  #[test]
  fn reads_the_captured_urls() {
    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    api.test_take_external_opens = Some(fake_take);
    api.string_free = Some(fake_string_free);
    assert_eq!(
      test_take_external_opens_with(&api),
      Some(vec![
        "https://a.example/".to_string(),
        "b\"c\\d".to_string(),
        "e\nf".to_string()
      ])
    );
  }

  #[test]
  fn parses_only_string_arrays() {
    assert_eq!(parse_string_array("[]"), Some(vec![]));
    assert_eq!(parse_string_array(" [\"x\"] "), Some(vec!["x".to_string()]));
    assert_eq!(parse_string_array("[1]"), None);
    assert_eq!(parse_string_array("[\"x\""), None);
    assert_eq!(parse_string_array("{}"), None);
    assert_eq!(parse_string_array("[\"x\",]"), None);
  }
}
