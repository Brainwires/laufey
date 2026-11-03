// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

use std::env;
use std::path::PathBuf;

#[path = "build_support.rs"]
mod build_support;

fn main() {
  let header = "include/laufey.h";
  println!("cargo:rerun-if-changed={}", header);
  println!("cargo:rerun-if-changed=build_support.rs");

  let mut builder = bindgen::Builder::default()
    .header(header)
    .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
    .allowlist_type("laufey_.*")
    .allowlist_var("LAUFEY_.*");

  // The target's, not the host's: build_support.rs.
  let target_os = env::var("CARGO_CFG_TARGET_OS").unwrap_or_default();
  let target_arch = env::var("CARGO_CFG_TARGET_ARCH").unwrap_or_default();
  builder = builder
    .clang_args(build_support::bindgen_clang_args(&target_os, &target_arch));

  let bindings = builder.generate().expect("Unable to generate bindings");

  let out_path = PathBuf::from(env::var("OUT_DIR").unwrap());
  bindings
    .write_to_file(out_path.join("bindings.rs"))
    .expect("Couldn't write bindings!");
}
