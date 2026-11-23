// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The native tree (API 61): a window hosts a retained tree of native
//! primitives that the runtime mutates in batches. See `docs/native-tree.md`.
//!
//! A backend with a native tree renderer (the GPUI host) fills the
//! `native_tree_*` entries of the API table; every other backend leaves them
//! `NULL`, and everything here then answers "unsupported".

use std::ffi::{c_char, c_void};
use std::sync::{Arc, Mutex, OnceLock};

use crate::{api, LaufeyBackendApi, Window, WindowCapabilities};

/// `native_tree_commit` answers (`LAUFEY_NATIVE_TREE_*`).
pub const LAUFEY_NATIVE_TREE_OK: i32 = 0;
pub const LAUFEY_NATIVE_TREE_UNSUPPORTED: i32 = -1;
pub const LAUFEY_NATIVE_TREE_MALFORMED: i32 = -2;
pub const LAUFEY_NATIVE_TREE_NO_WINDOW: i32 = -3;

/// What [`WindowCapabilities::native_tree`] reports: the batch format the
/// backend takes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NativeTreeCapability {
  /// The batch format version (`1` is "NTB1").
  pub version: u32,
}

/// Why [`NativeTree::commit`] did not take a batch.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NativeTreeError {
  /// This backend has no native tree.
  Unsupported,
  /// The batch does not decode; nothing of it was applied.
  Malformed,
  /// No such window.
  NoWindow,
  /// An answer this crate doesn't know.
  Other(i32),
}

impl NativeTreeError {
  fn from_raw(code: i32) -> Self {
    match code {
      LAUFEY_NATIVE_TREE_UNSUPPORTED => NativeTreeError::Unsupported,
      LAUFEY_NATIVE_TREE_MALFORMED => NativeTreeError::Malformed,
      LAUFEY_NATIVE_TREE_NO_WINDOW => NativeTreeError::NoWindow,
      other => NativeTreeError::Other(other),
    }
  }
}

impl std::fmt::Display for NativeTreeError {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match self {
      NativeTreeError::Unsupported => {
        f.write_str("this backend has no native tree")
      }
      NativeTreeError::Malformed => f.write_str("malformed native tree batch"),
      NativeTreeError::NoWindow => f.write_str("no such window"),
      NativeTreeError::Other(code) => {
        write!(f, "native tree commit failed ({code})")
      }
    }
  }
}

impl std::error::Error for NativeTreeError {}

type Handler = Arc<dyn Fn(u32, &str) + Send + Sync>;

fn handler_slot() -> &'static Mutex<Option<Handler>> {
  static SLOT: OnceLock<Mutex<Option<Handler>>> = OnceLock::new();
  SLOT.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn trampoline(
  _user_data: *mut c_void,
  window_id: u32,
  json: *const c_char,
  len: usize,
) {
  if json.is_null() {
    return;
  }
  // SAFETY: the backend's contract: `len` bytes, valid for this call.
  let bytes = unsafe { std::slice::from_raw_parts(json.cast::<u8>(), len) };
  let Ok(json) = std::str::from_utf8(bytes) else {
    return;
  };
  let handler = handler_slot().lock().unwrap().clone();
  if let Some(handler) = handler {
    handler(window_id, json);
  }
}

/// The native tree of a window. All of it may be called from any thread.
pub struct NativeTree;

impl NativeTree {
  /// The batch format version this backend takes, or `None` when it has no
  /// native tree.
  pub fn version() -> Option<u32> {
    Self::version_with(api())
  }

  fn version_with(api: &LaufeyBackendApi) -> Option<u32> {
    let f = api.native_tree_version?;
    api.native_tree_commit?;
    match unsafe { f(api.backend_data) } {
      0 => None,
      version => Some(version),
    }
  }

  /// Apply one batch to `window`'s tree. The backend copies `batch` and
  /// applies it whole on the UI thread before the next frame; a window's
  /// batches apply in commit order. `Ok` means the batch was taken: one that
  /// decodes but does not fit the tree is reported as a `batchError` event.
  pub fn commit(window: &Window, batch: &[u8]) -> Result<(), NativeTreeError> {
    Self::commit_with(api(), window.id(), batch)
  }

  fn commit_with(
    api: &LaufeyBackendApi,
    window_id: u32,
    batch: &[u8],
  ) -> Result<(), NativeTreeError> {
    let Some(f) = api.native_tree_commit else {
      return Err(NativeTreeError::Unsupported);
    };
    match unsafe { f(api.backend_data, window_id, batch.as_ptr(), batch.len()) }
    {
      LAUFEY_NATIVE_TREE_OK => Ok(()),
      code => Err(NativeTreeError::from_raw(code)),
    }
  }

  /// Register the (single, process-wide) handler for the native tree events
  /// of every window: the window's id and one JSON object
  /// (`{"node":id,"type":...}`). Fires on the UI thread. Replaces any
  /// previous handler. Returns `false`, having kept the handler for nothing,
  /// when the backend has no native tree.
  pub fn on_event<F>(handler: F) -> bool
  where
    F: Fn(u32, &str) + Send + Sync + 'static,
  {
    Self::on_event_with(api(), handler)
  }

  fn on_event_with<F>(api: &LaufeyBackendApi, handler: F) -> bool
  where
    F: Fn(u32, &str) + Send + Sync + 'static,
  {
    *handler_slot().lock().unwrap() = Some(Arc::new(handler));
    match api.set_native_tree_event_handler {
      Some(f) => {
        unsafe { f(api.backend_data, Some(trampoline), std::ptr::null_mut()) };
        true
      }
      None => false,
    }
  }

  /// `window`'s tree as JSON (see `native_tree_snapshot` in `laufey.h`), or
  /// `None` for an unknown window or a backend without a native tree. For
  /// self-tests.
  pub fn snapshot(window: &Window) -> Option<String> {
    let api = api();
    let f = api.native_tree_snapshot?;
    unsafe {
      crate::io::take_backend_string(api, f(api.backend_data, window.id()))
    }
  }
}

impl WindowCapabilities {
  /// The backend's native tree (API 61), or `None` when it has none (CEF,
  /// WebView, Winit). Asks the backend for the version: any thread.
  pub fn native_tree(&self) -> Option<NativeTreeCapability> {
    self.native_tree_with(api())
  }

  fn native_tree_with(
    &self,
    api: &LaufeyBackendApi,
  ) -> Option<NativeTreeCapability> {
    if !self.has(crate::LAUFEY_WINDOW_CAP_NATIVE_TREE) {
      return None;
    }
    NativeTree::version_with(api)
      .map(|version| NativeTreeCapability { version })
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::ffi;
  use std::sync::atomic::{AtomicUsize, Ordering};

  #[test]
  fn the_constants_match_the_header() {
    assert_eq!(LAUFEY_NATIVE_TREE_OK, ffi::LAUFEY_NATIVE_TREE_OK as i32);
    assert_eq!(
      LAUFEY_NATIVE_TREE_UNSUPPORTED,
      ffi::LAUFEY_NATIVE_TREE_UNSUPPORTED
    );
    assert_eq!(
      LAUFEY_NATIVE_TREE_MALFORMED,
      ffi::LAUFEY_NATIVE_TREE_MALFORMED
    );
    assert_eq!(
      LAUFEY_NATIVE_TREE_NO_WINDOW,
      ffi::LAUFEY_NATIVE_TREE_NO_WINDOW
    );
    assert_eq!(
      crate::LAUFEY_WINDOW_CAP_NATIVE_TREE,
      ffi::LAUFEY_WINDOW_CAP_NATIVE_TREE
    );
  }

  #[test]
  fn a_backend_without_the_entries_is_unsupported() {
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    assert_eq!(NativeTree::version_with(&none), None);
    assert_eq!(
      NativeTree::commit_with(&none, 1, b"NTB1"),
      Err(NativeTreeError::Unsupported)
    );
    assert!(!NativeTree::on_event_with(&none, |_, _| {}));
    // The bit alone is not a native tree.
    let caps = WindowCapabilities {
      bits: crate::LAUFEY_WINDOW_CAP_NATIVE_TREE,
    };
    assert_eq!(caps.native_tree_with(&none), None);
    assert_eq!(WindowCapabilities::default().native_tree_with(&none), None);
  }

  static COMMITTED: AtomicUsize = AtomicUsize::new(0);

  unsafe extern "C" fn fake_version(_: *mut c_void) -> u32 {
    1
  }

  unsafe extern "C" fn fake_commit(
    _: *mut c_void,
    window_id: u32,
    data: *const u8,
    len: usize,
  ) -> i32 {
    let bytes = unsafe { std::slice::from_raw_parts(data, len) };
    if window_id != 7 {
      return LAUFEY_NATIVE_TREE_NO_WINDOW;
    }
    if !bytes.starts_with(b"NTB1") {
      return LAUFEY_NATIVE_TREE_MALFORMED;
    }
    COMMITTED.fetch_add(len, Ordering::SeqCst);
    LAUFEY_NATIVE_TREE_OK
  }

  #[test]
  fn commit_and_capability_follow_the_backend() {
    let mut fake: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    fake.native_tree_version = Some(fake_version);
    fake.native_tree_commit = Some(fake_commit);
    assert_eq!(NativeTree::version_with(&fake), Some(1));
    assert_eq!(NativeTree::commit_with(&fake, 7, b"NTB1...."), Ok(()));
    assert_eq!(COMMITTED.load(Ordering::SeqCst), 8);
    assert_eq!(
      NativeTree::commit_with(&fake, 7, b"nope"),
      Err(NativeTreeError::Malformed)
    );
    assert_eq!(
      NativeTree::commit_with(&fake, 8, b"NTB1"),
      Err(NativeTreeError::NoWindow)
    );
    let caps = WindowCapabilities {
      bits: crate::LAUFEY_WINDOW_CAP_NATIVE_TREE,
    };
    assert_eq!(
      caps.native_tree_with(&fake),
      Some(NativeTreeCapability { version: 1 })
    );
    assert_eq!(NativeTreeError::from_raw(-9), NativeTreeError::Other(-9));
  }

  #[test]
  fn events_reach_the_handler_as_utf8() {
    static SEEN: Mutex<Vec<(u32, String)>> = Mutex::new(Vec::new());
    let none: LaufeyBackendApi = unsafe { std::mem::zeroed() };
    NativeTree::on_event_with(&none, |window, json| {
      SEEN.lock().unwrap().push((window, json.to_string()));
    });
    let json = r#"{"node":3,"type":"press","x":1,"y":2}"#;
    unsafe {
      trampoline(std::ptr::null_mut(), 5, json.as_ptr().cast(), json.len());
      // Not UTF-8, and NULL: dropped.
      trampoline(std::ptr::null_mut(), 5, [0xffu8].as_ptr().cast(), 1);
      trampoline(std::ptr::null_mut(), 5, std::ptr::null(), 0);
    }
    assert_eq!(*SEEN.lock().unwrap(), vec![(5, json.to_string())]);
  }
}
