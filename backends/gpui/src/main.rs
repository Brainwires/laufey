//! B0 spike: one process with a GPUI window and a deno_core isolate from the Brainwires/deno
//! fork (docs in `../SPIKE.md`).
//!
//! ```text
//! laufey-gpui-spike [--self-test] [--presses N]
//! ```
//!
//! Without flags it opens the window and runs the embedded app (`app.js`): a title and a
//! button that counts its clicks. JS commits a UI tree through `op_gpui_commit`, GPUI draws it,
//! a click resolves JS's pending `op_gpui_next_event`, and JS commits the next tree.
//!
//! `--self-test` sends `--presses` (default 20) press events through the same channel a click
//! uses, checks the counter in every recommitted tree, prints a `b0-result` line and exits 0,
//! or 1 when a tree is wrong or nothing comes back in time. It still opens the window: GPUI's
//! headless macOS platform has no windows, and the frame timings need one.
//!
//! Threads: GPUI's `App` on the main thread; a "js" thread with a current-thread tokio runtime
//! and the `JsRuntime`. Trees go JS -> UI and events UI -> JS, each on its own channel.

mod js;
mod tree;
mod ui;

use std::time::Instant;

use anyhow::{Context as _, bail};

fn main() -> anyhow::Result<()> {
  let launched_at = Instant::now();
  let mut self_test = false;
  let mut presses = 20u32;
  let mut args = std::env::args().skip(1);
  while let Some(arg) = args.next() {
    match arg.as_str() {
      "--self-test" => self_test = true,
      "--presses" => {
        presses = args
          .next()
          .context("--presses needs a value")?
          .parse()
          .context("--presses")?;
      }
      "-h" | "--help" => {
        println!("laufey-gpui-spike [--self-test] [--presses N]");
        return Ok(());
      }
      other => bail!("unknown argument {other}"),
    }
  }

  // V8's platform is process-wide and must be set up on the main thread before any isolate
  // exists on another one (its memory-protection keys are inherited by threads made later).
  deno_core::JsRuntime::init_platform(None);

  let (commits, commit_receiver) = futures::channel::mpsc::unbounded();
  let js = js::JsHost::spawn(commits)?;
  ui::run(
    js,
    commit_receiver,
    launched_at,
    self_test.then(|| ui::SelfTest::new(presses)),
  );
  Ok(())
}
