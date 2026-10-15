// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! User activities (API 49): Handoff and Spotlight through NSUserActivity on
//! macOS. See `docs/user-activity.md`.

use std::collections::BTreeMap;
use std::ffi::{c_char, c_void, CStr, CString};
use std::sync::{Arc, Mutex, OnceLock};

use crate::{api, LaufeyBackendApi};

/// An activity the app is engaged in.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct UserActivity {
  /// Reverse-DNS; listed in the bundle's `NSUserActivityTypes` for another
  /// device (or Spotlight) to continue it.
  pub activity_type: String,
  pub title: String,
  /// An http(s) URL a device without the app opens in a browser.
  pub url: Option<String>,
  pub user_info: BTreeMap<String, String>,
  pub keywords: Vec<String>,
  /// Offered to the user's other devices (default true).
  pub handoff: bool,
  /// Indexed by Spotlight (default false).
  pub searchable: bool,
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

impl UserActivity {
  fn to_json(&self) -> String {
    let mut out = format!(
      "{{\"type\":{},\"title\":{},\"handoff\":{},\"searchable\":{}",
      json_string(&self.activity_type),
      json_string(&self.title),
      self.handoff,
      self.searchable
    );
    if let Some(url) = &self.url {
      out.push_str(&format!(",\"url\":{}", json_string(url)));
    }
    let info: Vec<String> = self
      .user_info
      .iter()
      .map(|(k, v)| format!("{}:{}", json_string(k), json_string(v)))
      .collect();
    out.push_str(&format!(",\"userInfo\":{{{}}}", info.join(",")));
    let keywords: Vec<String> =
      self.keywords.iter().map(|k| json_string(k)).collect();
    out.push_str(&format!(",\"keywords\":[{}]}}", keywords.join(",")));
    out
  }
}

/// Make `activity` the app's current activity, or end the current one with
/// `None`. Returns `false` for an invalid activity and where there are none
/// (Windows, Linux, older than API 49).
pub fn set_user_activity(activity: Option<&UserActivity>) -> bool {
  set_user_activity_with(api(), activity)
}

fn set_user_activity_with(
  api: &LaufeyBackendApi,
  activity: Option<&UserActivity>,
) -> bool {
  let Some(f) = api.set_user_activity else {
    return false;
  };
  match activity {
    None => unsafe { f(api.backend_data, std::ptr::null()) },
    Some(a) => match CString::new(a.to_json()) {
      Ok(json) => unsafe { f(api.backend_data, json.as_ptr()) },
      Err(_) => false,
    },
  }
}

/// A continued activity's JSON: `{"type", "title", "url", "userInfo"}`.
type ActivityHandler = Arc<dyn Fn(String) + Send + Sync>;

fn handler_slot() -> &'static Mutex<Option<ActivityHandler>> {
  static SLOT: OnceLock<Mutex<Option<ActivityHandler>>> = OnceLock::new();
  SLOT.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn activity_trampoline(_: *mut c_void, json: *const c_char) {
  if json.is_null() {
    return;
  }
  let text = unsafe { CStr::from_ptr(json) }
    .to_string_lossy()
    .into_owned();
  let handler = handler_slot().lock().unwrap().clone();
  if let Some(handler) = handler {
    handler(text);
  }
}

/// Register the (process-wide) handler for activities the user continues
/// (Handoff, a Spotlight result), with each one's JSON. Ones continued
/// before a handler existed (the one that launched the app) arrive when it
/// registers. Returns `false` where there are no user activities.
pub fn on_user_activity<F>(handler: F) -> bool
where
  F: Fn(String) + Send + Sync + 'static,
{
  *handler_slot().lock().unwrap() = Some(Arc::new(handler));
  let api = api();
  match api.set_user_activity_handler {
    Some(f) => {
      unsafe {
        f(
          api.backend_data,
          Some(activity_trampoline),
          std::ptr::null_mut(),
        )
      };
      true
    }
    None => false,
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  static LAST: Mutex<Option<String>> = Mutex::new(None);

  unsafe extern "C" fn fake_set(_: *mut c_void, json: *const c_char) -> bool {
    *LAST.lock().unwrap() = (!json.is_null()).then(|| {
      unsafe { CStr::from_ptr(json) }
        .to_string_lossy()
        .into_owned()
    });
    true
  }

  #[test]
  fn not_supported_without_the_entry_point() {
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert!(!set_user_activity_with(&none, None));
  }

  #[test]
  fn sends_the_activity_and_the_clear() {
    let mut api: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    api.set_user_activity = Some(fake_set);
    let mut info = BTreeMap::new();
    info.insert("id".to_string(), "42".to_string());
    let activity = UserActivity {
      activity_type: "com.example.view".into(),
      title: "Doc \"42\"".into(),
      url: Some("https://example.com/d/42".into()),
      user_info: info,
      keywords: vec!["doc".into()],
      handoff: true,
      searchable: true,
    };
    assert!(set_user_activity_with(&api, Some(&activity)));
    assert_eq!(
      LAST.lock().unwrap().as_deref(),
      Some(
        r#"{"type":"com.example.view","title":"Doc \"42\"","handoff":true,"searchable":true,"url":"https://example.com/d/42","userInfo":{"id":"42"},"keywords":["doc"]}"#
      )
    );
    assert!(set_user_activity_with(&api, None));
    assert_eq!(*LAST.lock().unwrap(), None);
  }
}
