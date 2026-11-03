// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Decisions build.rs makes for the target being built. Shared with the
//! crate's unit tests (`#[path]` from src/lib.rs), since a build script has
//! no tests of its own.

/// The extra clang arguments laufey.h's bindings are generated with, for a
/// target `target_os` / `target_arch` (Cargo's `CARGO_CFG_TARGET_OS` /
/// `CARGO_CFG_TARGET_ARCH`). A build script runs on the host, so `cfg!`
/// there names the host: a cross build to Windows got none of these, and one
/// from Windows to elsewhere got them all.
///
/// Windows: parse the Windows SDK / MSVC headers the way MSVC does
/// (`__declspec` and other Microsoft extensions). laufey.h is C, where
/// `wchar_t` is not a builtin type: the UCRT headers typedef it themselves
/// unless `_WCHAR_T_DEFINED` is already set, so that macro (and
/// `_NATIVE_WCHAR_T_DEFINED`, which claims a builtin `wchar_t`) must not be
/// predefined — with it, Windows SDK 10.0.26100's corecrt.h fails with
/// "unknown type name 'wchar_t'".
pub fn bindgen_clang_args(target_os: &str, target_arch: &str) -> Vec<String> {
  if target_os != "windows" {
    return Vec::new();
  }
  // The real target triple, so clang parses with the correct arch on both x64
  // and ARM64 Windows (aarch64-pc-windows-msvc). Both are LLP64 so the
  // generated layouts match, but hardcoding x86_64 was still wrong.
  let clang_arch = match target_arch {
    "" => "x86_64",
    "x86" => "i686",
    arch => arch,
  };
  [
    "-fms-extensions",
    "-fms-compatibility",
    "-fdelayed-template-parsing",
    "-fmsc-version=1950",
    "-Wno-everything",
  ]
  .iter()
  .map(|a| a.to_string())
  .chain([format!("--target={clang_arch}-pc-windows-msvc")])
  .collect()
}
