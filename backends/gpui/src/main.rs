//! `laufey_gpui`: laufey's GPUI backend (docs/backends.md, "GPUI").
//!
//! A host executable like `laufey_winit`: it loads the app's runtime library, hands it the
//! API table, and runs the UI loop. It has no web engine and no JavaScript bridge; what it
//! draws is the runtime's **native tree** (docs/native-tree.md), a retained tree of views,
//! text, images, scroll views, text inputs and pressables that the runtime mutates in
//! batches, mapped onto GPUI elements. V8 lives in the runtime library, never here.
//!
//! ```text
//! laufey_gpui [--runtime <library>] [--self-test]
//! ```
//!
//! The runtime is `--runtime`, else `LAUFEY_RUNTIME_PATH`, else a library next to the
//! executable with its base name. `--self-test` runs the bundled test runtime
//! (`test-runtime/`) when no other is named; the run prints one `b1-result` line and exits
//! 0 only when every check passed.
//!
//! Threads: GPUI on the main thread; the runtime on its own ("laufey-runtime"), started
//! after `laufey_runtime_prestart(3)` and `laufey_runtime_init` ran on the main thread.

mod api;
mod host;
mod input;
mod runtime;
mod style;
mod ui;
mod view;

use std::io::Write as _;
use std::path::PathBuf;
use std::time::Duration;

use gpui::{App, QuitMode};

/// How long a self-test may take before it is failed.
const SELF_TEST_TIMEOUT: Duration = Duration::from_secs(60);

struct Args {
  runtime: Option<PathBuf>,
  self_test: bool,
}

fn parse_args() -> Args {
  let mut parsed = Args {
    runtime: None,
    self_test: false,
  };
  let mut args = std::env::args().skip(1);
  while let Some(arg) = args.next() {
    if arg == "--runtime" {
      parsed.runtime = args.next().map(PathBuf::from);
    } else if let Some(path) = arg.strip_prefix("--runtime=") {
      parsed.runtime = Some(PathBuf::from(path));
    } else if arg == "--self-test" {
      parsed.self_test = true;
    } else if arg == "-h" || arg == "--help" {
      println!("laufey_gpui [--runtime <library>] [--self-test]");
      std::process::exit(0);
    }
    // Anything else is the app's: the runtime reads the process arguments itself.
  }
  parsed
}

/// A failure before the runtime runs. The self-test's contract is one `b1-result` line.
fn fail(self_test: bool, reason: &str) -> ! {
  eprintln!("laufey_gpui: {reason}");
  if self_test {
    println!("b1-result status=fail reason=host detail={reason:?}");
  }
  let _ = std::io::stdout().flush();
  std::process::exit(1);
}

/// The app ends: stop the runtime and end the process with `exit_app`'s code, else 0.
///
/// Called directly for `quit` / `exit_app` and the last window closing (`host::quit`), so
/// those never go through the platform's own quit (on macOS `terminate:`, whose callbacks
/// run under AppKit), and from the two places a quit the platform started ends up in.
pub fn finish() -> ! {
  runtime::shutdown();
  let _ = std::io::stdout().flush();
  let _ = std::io::stderr().flush();
  std::process::exit(host::requested_exit_code().unwrap_or(0));
}

/// A panic anywhere in the host ends the process with a message and exit code 101, at once.
/// GPUI calls this host from AppKit callbacks (launch, events, draw); a panic that unwinds
/// into one aborts the process (`SIGABRT`, a crash report and the "quit unexpectedly"
/// dialog) instead.
fn exit_on_panic(self_test: bool) {
  let default_hook = std::panic::take_hook();
  std::panic::set_hook(Box::new(move |info| {
    default_hook(info);
    if self_test {
      println!("b1-result status=fail reason=host-panic");
    }
    let _ = std::io::stdout().flush();
    let _ = std::io::stderr().flush();
    std::process::exit(101);
  }));
}

fn main() {
  let args = parse_args();
  exit_on_panic(args.self_test);
  ui::bind_current_thread();

  let path = runtime::find(args.runtime.clone())
    .or_else(|| args.self_test.then(runtime::self_test_runtime).flatten());
  let Some(path) = path else {
    fail(
      args.self_test,
      "no runtime library: pass --runtime <path>, set LAUFEY_RUNTIME_PATH, or place it \
       next to the executable under the executable's name",
    );
  };
  let runtime = match runtime::Runtime::load(path) {
    Ok(runtime) => runtime,
    Err(reason) => fail(args.self_test, &reason),
  };
  // First of all, while the process has one thread: the runtime may confine it here.
  if let Err(code) = runtime.prestart() {
    std::process::exit(code);
  }
  if let Err(reason) = runtime.start(api::table()) {
    fail(args.self_test, &reason);
  }

  if args.self_test {
    std::thread::spawn(|| {
      std::thread::sleep(SELF_TEST_TIMEOUT);
      println!(
        "b1-result status=fail reason=timeout detail=\"no result after {}s\"",
        SELF_TEST_TIMEOUT.as_secs()
      );
      let _ = std::io::stdout().flush();
      std::process::exit(1);
    });
  }

  gpui_platform::application()
    // The host decides when the app ends (quit, exit_app, the last window closing).
    .with_quit_mode(QuitMode::Explicit)
    .run(|cx: &mut App| {
      input::bind_keys(cx);
      cx.on_window_closed(host::gpui_window_closed).detach();
      // A quit the platform started (Cmd+Q, the Dock, a logout). Where it ends the process
      // inside its own quit (macOS's `terminate:`), this is the last code that runs.
      cx.on_app_quit(|_| {
        finish();
        #[allow(unreachable_code)]
        async {}
      })
      .detach();
      ui::start_pump(cx);
    });
  // Where `run` returns once the loop has ended.
  finish();
}
