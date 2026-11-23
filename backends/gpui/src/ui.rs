//! The UI thread's queue: how every other thread, and the UI thread's own event handlers,
//! get work onto GPUI's main thread.
//!
//! Two kinds of job, in one FIFO:
//!
//! - [`Job::Ui`]: host work that needs the [`App`] (create a window, apply a batch). It never
//!   calls into the runtime.
//! - [`Job::Call`]: a call into the runtime (an event handler, a `dispatch_ui_task` task).
//!   The runtime may call straight back into the API table from inside it, on this thread;
//!   those calls find the `App` through [`with_app`], which the pump arms for the length of
//!   the call. A `Call` job itself holds no `App` borrow, so the re-entrant use is the only
//!   one.
//!
//! Runtime callbacks are only ever made from the pump, never from inside a GPUI element or
//! view callback: there a window or view is borrowed, and a re-entrant API call that touched
//! it would fail. Producers of events therefore [`post_call`] them.
//!
//! Once the loop has ended ([`close`]) queued jobs are dropped unrun, which releases anyone
//! blocked in [`run_sync`] (their reply channel closes), and later jobs are dropped at once.

use std::cell::Cell;
use std::collections::VecDeque;
use std::sync::Mutex;
use std::sync::mpsc;
use std::thread::ThreadId;

use futures::StreamExt as _;
use futures::channel::mpsc::{UnboundedReceiver, UnboundedSender, unbounded};
use gpui::App;

enum Job {
  Ui(Box<dyn FnOnce(&mut App) + Send>),
  Call(Box<dyn FnOnce() + Send>),
}

struct Queue {
  jobs: VecDeque<Job>,
  closed: bool,
  waker: Option<UnboundedSender<()>>,
  ui_thread: Option<ThreadId>,
}

static QUEUE: Mutex<Queue> = Mutex::new(Queue {
  jobs: VecDeque::new(),
  closed: false,
  waker: None,
  ui_thread: None,
});

fn queue() -> std::sync::MutexGuard<'static, Queue> {
  QUEUE.lock().unwrap_or_else(|e| e.into_inner())
}

thread_local! {
  /// The `App`, while the pump runs a [`Job::Call`] on this (the UI) thread.
  static APP: Cell<*mut App> = const { Cell::new(std::ptr::null_mut()) };
}

/// Record the calling thread as the UI thread. Call on the main thread before the runtime
/// starts.
pub fn bind_current_thread() {
  queue().ui_thread = Some(std::thread::current().id());
}

/// True on the thread GPUI runs on.
pub fn is_ui_thread() -> bool {
  queue().ui_thread == Some(std::thread::current().id())
}

fn push(job: Job) {
  let mut q = queue();
  if q.closed {
    drop(q);
    // Dropped outside the lock: a job's drop may release a waiter that posts again.
    drop(job);
    return;
  }
  q.jobs.push_back(job);
  if let Some(waker) = &q.waker {
    let _ = waker.unbounded_send(());
  }
}

/// Run `f` with the `App` when the calling thread is the UI thread inside a runtime callback;
/// otherwise hand `f` back.
fn with_app<R, F: FnOnce(&mut App) -> R>(f: F) -> Result<R, F> {
  let app = APP.with(Cell::get);
  if app.is_null() {
    return Err(f);
  }
  // SAFETY: the pump stored its `&mut App` here and does not touch it until the runtime
  // callback it is running returns; this call is inside that callback, on the same thread,
  // and the slot is emptied while `f` runs so a nested call cannot alias it.
  APP.with(|slot| slot.set(std::ptr::null_mut()));
  let result = f(unsafe { &mut *app });
  APP.with(|slot| slot.set(app));
  Ok(result)
}

/// Run `f` on the UI thread, after everything already queued; do not wait for it. On the UI
/// thread inside a runtime callback it runs at once.
pub fn post(f: impl FnOnce(&mut App) + Send + 'static) {
  if is_ui_thread() {
    match with_app(f) {
      Ok(()) => {}
      Err(f) => push(Job::Ui(Box::new(f))),
    }
    return;
  }
  push(Job::Ui(Box::new(f)));
}

/// Run `f` on the UI thread and wait for its result. `None` when the loop has ended (or ends
/// first), and on the UI thread outside a runtime callback, where waiting would deadlock.
pub fn run_sync<R: Send + 'static>(
  f: impl FnOnce(&mut App) -> R + Send + 'static,
) -> Option<R> {
  if is_ui_thread() {
    return with_app(f).ok();
  }
  let (tx, rx) = mpsc::sync_channel(1);
  push(Job::Ui(Box::new(move |app| {
    let _ = tx.send(f(app));
  })));
  // An unrun job drops `tx`, which ends the wait.
  rx.recv().ok()
}

/// Queue a call into the runtime (an event handler, a UI task). Always queued, also from the
/// UI thread: see the module docs.
pub fn post_call(f: impl FnOnce() + Send + 'static) {
  push(Job::Call(Box::new(f)));
}

fn run(job: Job, app: &mut App) {
  match job {
    Job::Ui(f) => f(app),
    Job::Call(f) => {
      let previous = APP.with(|slot| slot.replace(app as *mut App));
      // The runtime's code: a panic of its own that unwinds this far (a Rust runtime built
      // to unwind) stops here instead of crossing GPUI's and the platform's frames.
      let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(f));
      APP.with(|slot| slot.set(previous));
      if outcome.is_err() {
        eprintln!("laufey_gpui: a runtime callback panicked on the UI thread");
      }
    }
  }
}

/// Start draining the queue on GPUI's foreground executor. Call once, from
/// `Application::run`'s callback.
pub fn start_pump(cx: &mut App) {
  let (waker, mut wakes): (UnboundedSender<()>, UnboundedReceiver<()>) =
    unbounded();
  {
    let mut q = queue();
    q.waker = Some(waker.clone());
    if !q.jobs.is_empty() {
      let _ = waker.unbounded_send(());
    }
  }
  cx.spawn(async move |cx| {
    while wakes.next().await.is_some() {
      loop {
        // One job per lock: a job may push more.
        let Some(job) = queue().jobs.pop_front() else {
          break;
        };
        cx.update(|app| run(job, app));
      }
    }
  })
  .detach();
}

/// The loop has ended: drop every queued job and every later one. Idempotent.
pub fn close() {
  let jobs = {
    let mut q = queue();
    q.closed = true;
    q.waker = None;
    std::mem::take(&mut q.jobs)
  };
  drop(jobs);
}

/// `dispatch_ui_task` (API 42): tasks with a delivery guarantee, the twin of
/// `backend-winit-common/src/ui_tasks.rs`. Every task is called exactly once: with `true` on
/// the UI thread, or with `false` once the loop has ended.
pub mod tasks {
  use std::collections::HashMap;
  use std::ffi::c_void;
  use std::sync::Mutex;

  /// `laufey_ui_task_fn`.
  pub type UiTaskFn = unsafe extern "C" fn(*mut c_void, bool);

  struct State {
    closed: bool,
    next_id: usize,
    pending: Option<HashMap<usize, (UiTaskFn, usize)>>,
  }

  static STATE: Mutex<State> = Mutex::new(State {
    closed: false,
    next_id: 0,
    pending: None,
  });

  fn state() -> std::sync::MutexGuard<'static, State> {
    STATE.lock().unwrap_or_else(|e| e.into_inner())
  }

  fn take(id: usize) -> Option<(UiTaskFn, usize)> {
    state().pending.as_mut().and_then(|p| p.remove(&id))
  }

  /// See `dispatch_ui_task` in laufey.h.
  pub fn dispatch(task: UiTaskFn, data: usize) {
    let id = {
      let mut s = state();
      if s.closed {
        None
      } else {
        s.next_id += 1;
        let id = s.next_id;
        s.pending
          .get_or_insert_with(HashMap::new)
          .insert(id, (task, data));
        Some(id)
      }
    };
    let Some(id) = id else {
      // SAFETY: the caller's task, called exactly once.
      unsafe { task(data as *mut c_void, false) };
      return;
    };
    // Queued behind the work already posted, never inline. If the queue drops the job
    // (the loop ended), `close` below answers the task.
    super::post_call(move || {
      if let Some((task, data)) = take(id) {
        // SAFETY: the caller's task, called exactly once (it was still pending).
        unsafe { task(data as *mut c_void, true) };
      }
    });
  }

  /// The loop has ended: answer every pending task with `false` (on this thread) and every
  /// later dispatch at once. Idempotent.
  pub fn close() {
    let pending = {
      let mut s = state();
      s.closed = true;
      s.pending.take()
    };
    for (_, (task, data)) in pending.into_iter().flatten() {
      // SAFETY: as in `dispatch`.
      unsafe { task(data as *mut c_void, false) };
    }
  }
}
