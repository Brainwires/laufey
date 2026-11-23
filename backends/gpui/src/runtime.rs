//! Loading and starting the app runtime (docs/c-abi.md, "Runtime entry points").
//!
//! The runtime is a shared library (`libdenort` for a Denext app). The host loads it on the
//! main thread, calls its optional `laufey_runtime_prestart` there before anything else has
//! started (the runtime applies its sandbox in it, which on Linux holds only threads made
//! afterwards), then `laufey_runtime_init` with the API table, and `laufey_runtime_start` on
//! a thread of its own: the main thread belongs to GPUI.

use std::ffi::c_int;
use std::path::PathBuf;
use std::sync::{Mutex, OnceLock};
use std::thread;
use std::time::Duration;

use libloading::Library;

use crate::{host, ui};

/// This host's engine id for `laufey_runtime_prestart` (1 is the OS web view, 2 is CEF).
pub const ENGINE_GPUI: c_int = 3;

type PrestartFn = unsafe extern "C" fn(c_int, *mut c_int) -> c_int;
type InitFn = unsafe extern "C" fn(*const laufey::LaufeyBackendApi) -> c_int;
type StartFn = unsafe extern "C" fn() -> c_int;
type ShutdownFn = unsafe extern "C" fn();

/// How long the runtime's thread gets to return from `laufey_runtime_start` once the loop
/// has ended (Winit's bound), and after `exit_app`, whose caller may never return.
const SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(5);
const EXIT_TIMEOUT: Duration = Duration::from_millis(200);

static SHUTDOWN: OnceLock<ShutdownFn> = OnceLock::new();
static THREAD: Mutex<Option<thread::JoinHandle<()>>> = Mutex::new(None);

/// A loaded runtime, before it is started.
pub struct Runtime {
  /// Never unloaded: the runtime's threads may outlive every use of it here.
  library: &'static Library,
  path: PathBuf,
}

fn library_extension() -> &'static str {
  if cfg!(target_os = "windows") {
    "dll"
  } else if cfg!(target_os = "macos") {
    "dylib"
  } else {
    "so"
  }
}

/// A library next to the executable that shares its base name (`laufey_gpui` ->
/// `laufey_gpui.dylib`, `MyApp.exe` -> `MyApp.dll`), as the other hosts look for.
fn colocated() -> Option<PathBuf> {
  let exe = std::env::current_exe().ok()?;
  let candidate = exe
    .parent()?
    .join(exe.file_stem()?)
    .with_extension(library_extension());
  (candidate.exists() && candidate != exe).then_some(candidate)
}

/// The self-test's runtime, built next to the host by the same cargo workspace.
pub fn self_test_runtime() -> Option<PathBuf> {
  let exe = std::env::current_exe().ok()?;
  let name = if cfg!(target_os = "windows") {
    "laufey_gpui_test_runtime.dll".to_string()
  } else {
    format!("liblaufey_gpui_test_runtime.{}", library_extension())
  };
  let candidate = exe.parent()?.join(name);
  candidate.exists().then_some(candidate)
}

/// Where the runtime is: `--runtime <path>` (or `--runtime=<path>`), then
/// `LAUFEY_RUNTIME_PATH`, then a co-located library.
pub fn find(argument: Option<PathBuf>) -> Option<PathBuf> {
  if let Some(path) = argument {
    return path.exists().then_some(path);
  }
  if let Some(path) = std::env::var_os("LAUFEY_RUNTIME_PATH") {
    let path = PathBuf::from(path);
    if path.exists() {
      return Some(path);
    }
  }
  colocated()
}

impl Runtime {
  pub fn load(path: PathBuf) -> Result<Self, String> {
    // SAFETY: loading the app's runtime library runs its initializers; that is the point.
    let library = unsafe { Library::new(&path) }
      .map_err(|e| format!("failed to load runtime {}: {e}", path.display()))?;
    Ok(Self {
      library: Box::leak(Box::new(library)),
      path,
    })
  }

  /// The runtime's prestart, when it exports one. `Err(code)`: the process must exit with
  /// `code` now. Call on the main thread before any other thread exists.
  pub fn prestart(&self) -> Result<(), i32> {
    // SAFETY: the symbol's documented signature (laufey.h, `laufey_runtime_prestart_fn`).
    let Ok(prestart) =
      (unsafe { self.library.get::<PrestartFn>(b"laufey_runtime_prestart\0") })
    else {
      return Ok(());
    };
    let mut code: c_int = 0;
    // SAFETY: as above; `code` is writable.
    if unsafe { prestart(ENGINE_GPUI, &mut code) } != 0 {
      Ok(())
    } else {
      Err(code)
    }
  }

  /// `laufey_runtime_init`, then `laufey_runtime_start` on the runtime's thread.
  pub fn start(
    self,
    api: &'static laufey::LaufeyBackendApi,
  ) -> Result<(), String> {
    let symbol = |name: &'static [u8]| {
      let text = String::from_utf8_lossy(&name[..name.len() - 1]).into_owned();
      format!("{} exports no {text}", self.path.display())
    };
    // SAFETY: the symbols' documented signatures (laufey.h).
    let (init, start) = unsafe {
      let init = *self
        .library
        .get::<InitFn>(b"laufey_runtime_init\0")
        .map_err(|_| symbol(b"laufey_runtime_init\0"))?;
      let start = *self
        .library
        .get::<StartFn>(b"laufey_runtime_start\0")
        .map_err(|_| symbol(b"laufey_runtime_start\0"))?;
      if let Ok(shutdown) =
        self.library.get::<ShutdownFn>(b"laufey_runtime_shutdown\0")
      {
        let _ = SHUTDOWN.set(*shutdown);
      }
      (init, start)
    };
    // SAFETY: the table is static and complete.
    let result = unsafe { init(api) };
    if result != 0 {
      return Err(format!("runtime init failed with code {result}"));
    }
    let handle = thread::Builder::new()
      .name("laufey-runtime".into())
      .spawn(move || {
        // SAFETY: after a successful init, once.
        let result = unsafe { start() };
        if result != 0 {
          eprintln!("laufey_gpui: runtime start returned {result}");
        }
      })
      .map_err(|e| format!("failed to start the runtime thread: {e}"))?;
    *THREAD.lock().unwrap_or_else(|e| e.into_inner()) = Some(handle);
    Ok(())
  }
}

/// The event loop has ended: answer what waits on the UI thread, tell the runtime, and wait
/// (bounded) for its thread. Idempotent.
pub fn shutdown() {
  // Tasks and jobs still queued can never run now: answer them first, so a runtime thread
  // waiting on one is released before it is waited for.
  ui::tasks::close();
  ui::close();
  let Some(handle) = THREAD.lock().unwrap_or_else(|e| e.into_inner()).take()
  else {
    return;
  };
  if let Some(shutdown) = SHUTDOWN.get() {
    // SAFETY: the runtime's shutdown entry point, after init.
    unsafe { shutdown() };
  }
  let timeout = if host::requested_exit_code().is_some() {
    EXIT_TIMEOUT
  } else {
    SHUTDOWN_TIMEOUT
  };
  let (done, wait) = std::sync::mpsc::channel();
  thread::spawn(move || {
    let _ = handle.join();
    let _ = done.send(());
  });
  let _ = wait.recv_timeout(timeout);
}
