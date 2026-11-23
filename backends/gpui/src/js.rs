//! The JS side: a dedicated thread that owns a current-thread tokio runtime and the deno_core
//! `JsRuntime`, and the two ops the app talks to the window through.
//!
//! - `op_gpui_commit(json)`: sync. Parses the tree and sends it to the UI thread.
//! - `op_gpui_next_event()`: async. Resolves with the next input event as JSON, or `null`
//!   once the UI has closed the event channel.

use std::cell::RefCell;
use std::rc::Rc;
use std::thread::JoinHandle;

use anyhow::Context as _;
use deno_core::{JsRuntime, OpState, RuntimeOptions, extension, op2};
use deno_error::JsErrorBox;
use futures::channel::mpsc::UnboundedSender as CommitSender;
use tokio::sync::mpsc::{
  UnboundedReceiver, UnboundedSender, unbounded_channel,
};

use crate::tree::{Node, UiEvent};

/// One committed tree: it replaces the one before it.
pub type Commit = Node;

type EventReceiver = Rc<tokio::sync::Mutex<UnboundedReceiver<UiEvent>>>;

#[op2(fast)]
fn op_gpui_commit(
  state: &mut OpState,
  #[string] json: &str,
) -> Result<(), JsErrorBox> {
  let commit: Commit = serde_json::from_str(json)
    .map_err(|err| JsErrorBox::type_error(format!("op_gpui_commit: {err}")))?;
  // A closed channel means the window is gone; the next op_gpui_next_event says so.
  let _ = state
    .borrow::<CommitSender<Commit>>()
    .unbounded_send(commit);
  Ok(())
}

#[op2]
#[string]
async fn op_gpui_next_event(state: Rc<RefCell<OpState>>) -> String {
  let events = state.borrow().borrow::<EventReceiver>().clone();
  let event = events.lock().await.recv().await;
  serde_json::to_string(&event).expect("a UiEvent serializes")
}

extension!(
    laufey_gpui,
    ops = [op_gpui_commit, op_gpui_next_event],
    options = {
        commits: CommitSender<Commit>,
        events: UnboundedReceiver<UiEvent>,
    },
    state = |state, options| {
        state.put(options.commits);
        state.put::<EventReceiver>(Rc::new(tokio::sync::Mutex::new(options.events)));
    },
);

/// The UI thread's handle on the JS thread.
pub struct JsHost {
  events: RefCell<Option<UnboundedSender<UiEvent>>>,
  thread: RefCell<Option<JoinHandle<()>>>,
}

impl JsHost {
  /// Start the JS thread and run `app.js` on it. Commits arrive on `commits`.
  ///
  /// Call after `JsRuntime::init_platform` has run on the main thread.
  pub fn spawn(commits: CommitSender<Commit>) -> anyhow::Result<Self> {
    let (events, event_receiver) = unbounded_channel();
    let thread = std::thread::Builder::new()
      .name("js".into())
      .spawn(move || {
        if let Err(err) = run(commits, event_receiver) {
          eprintln!("js thread: {err:#}");
          std::process::exit(2);
        }
      })
      .context("spawning the js thread")?;
    Ok(Self {
      events: RefCell::new(Some(events)),
      thread: RefCell::new(Some(thread)),
    })
  }

  /// Queue an input event for JS. Returns false once the JS side is gone.
  pub fn send(&self, event: UiEvent) -> bool {
    match self.events.borrow().as_ref() {
      Some(events) => events.send(event).is_ok(),
      None => false,
    }
  }

  /// Close the event channel, which ends the app's event loop, and wait for the isolate to
  /// be dropped on its own thread.
  pub fn shutdown(&self) {
    self.events.borrow_mut().take();
    if let Some(thread) = self.thread.borrow_mut().take() {
      let _ = thread.join();
    }
  }
}

fn run(
  commits: CommitSender<Commit>,
  events: UnboundedReceiver<UiEvent>,
) -> anyhow::Result<()> {
  let tokio = tokio::runtime::Builder::new_current_thread()
    .enable_time()
    .build()
    .context("building the js thread's tokio runtime")?;
  tokio.block_on(async move {
    let mut runtime = JsRuntime::new(RuntimeOptions {
      extensions: vec![laufey_gpui::init(commits, events)],
      ..Default::default()
    });
    runtime
      .execute_script("ext:laufey_gpui/app.js", include_str!("app.js"))
      .context("running app.js")?;
    runtime
      .run_event_loop(Default::default())
      .await
      .context("the js event loop")?;
    Ok(())
  })
}
