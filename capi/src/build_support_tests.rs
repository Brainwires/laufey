// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! build.rs picks laufey.h's bindgen flags from the target, not the host
//! (build_support.rs).

use crate::build_support::bindgen_clang_args;

#[test]
fn windows_targets_get_the_msvc_flags_whatever_the_host() {
  // Before, build.rs asked `cfg!(target_os = "windows")`, the host: a build
  // for Windows from macOS or Linux got none of these.
  let x64 = bindgen_clang_args("windows", "x86_64");
  assert!(x64.iter().any(|a| a == "-fms-extensions"));
  assert!(x64.iter().any(|a| a == "-fms-compatibility"));
  assert_eq!(x64.last().unwrap(), "--target=x86_64-pc-windows-msvc");
  assert_eq!(
    bindgen_clang_args("windows", "aarch64").last().unwrap(),
    "--target=aarch64-pc-windows-msvc"
  );
  assert_eq!(
    bindgen_clang_args("windows", "x86").last().unwrap(),
    "--target=i686-pc-windows-msvc"
  );
  assert_eq!(
    bindgen_clang_args("windows", "").last().unwrap(),
    "--target=x86_64-pc-windows-msvc"
  );
}

#[test]
fn other_targets_get_none_whatever_the_host() {
  // And a build from Windows for another target got them all.
  for os in ["macos", "linux", "ios", "android", ""] {
    assert!(bindgen_clang_args(os, "aarch64").is_empty(), "{os}");
  }
}
