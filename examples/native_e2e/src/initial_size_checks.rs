//! A window maps at the size it was created with (LAUFEY_E2E_ONLY=initial-size,
//! `--initial-size`), seen from the compositor: sway's window events and tree
//! (`swaymsg`, through `$SWAYSOCK`). A Wayland window's first size is the one
//! it has when it is shown, and a size asked for after that races the
//! compositor's first configure: on one loaded CPU, WebKitGTK kept the
//! backend's default size (800x600) for 24 of 60 frameless panels. So the first map must already be at
//! the size asked for, not only the size a moment later.
//!
//! Run it under a sway whose windows float (`HEADLESS_SWAY_FLOATING=1` for
//! scripts/headless-sway.sh): a tiled window takes the size of its tile.
//! `LAUFEY_E2E_INITIAL_SIZE_ROUNDS=N` repeats each window N times (default 3).
//! N/A where there is no sway to ask.

use std::io::BufReader;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{Window, WindowOptions};
use serde_json::Value;

use crate::{check, na, wait_for};

/// `swaymsg -t subscribe -m '["window"]'`: every window event, as they come.
struct WindowEvents {
  child: Child,
  events: Arc<Mutex<Vec<Value>>>,
}

impl WindowEvents {
  fn start() -> Option<Self> {
    let mut child = Command::new("swaymsg")
      .args(["-r", "-m", "-t", "subscribe", r#"["window"]"#])
      .stdin(Stdio::null())
      .stdout(Stdio::piped())
      .stderr(Stdio::null())
      .spawn()
      .ok()?;
    let stdout = child.stdout.take()?;
    let events = Arc::new(Mutex::new(Vec::new()));
    let sink = events.clone();
    std::thread::spawn(move || {
      let stream =
        serde_json::Deserializer::from_reader(BufReader::new(stdout))
          .into_iter::<Value>();
      for event in stream.flatten() {
        sink.lock().unwrap().push(event);
      }
    });
    Some(WindowEvents { child, events })
  }

  /// The container of the first `new` event after `from` events.
  fn new_since(&self, from: usize) -> Option<Value> {
    self.events.lock().unwrap()[from..]
      .iter()
      .find(|e| e["change"] == "new")
      .map(|e| e["container"].clone())
  }

  fn count(&self) -> usize {
    self.events.lock().unwrap().len()
  }
}

impl Drop for WindowEvents {
  fn drop(&mut self) {
    let _ = self.child.kill();
    let _ = self.child.wait();
  }
}

/// The node with this container id in sway's tree.
fn tree_node(id: u64) -> Option<Value> {
  let out = Command::new("swaymsg")
    .args(["-r", "-t", "get_tree"])
    .stdin(Stdio::null())
    .stderr(Stdio::null())
    .output()
    .ok()?;
  let tree: Value = serde_json::from_slice(&out.stdout).ok()?;
  fn find(node: &Value, id: u64) -> Option<Value> {
    if node["id"].as_u64() == Some(id) {
      return Some(node.clone());
    }
    for key in ["nodes", "floating_nodes"] {
      if let Some(children) = node[key].as_array() {
        for child in children {
          if let Some(found) = find(child, id) {
            return Some(found);
          }
        }
      }
    }
    None
  }
  find(&tree, id)
}

/// The surface's own size (its xdg geometry) as sway reports it.
fn geometry(node: &Value) -> Option<(i64, i64)> {
  let g = &node["geometry"];
  Some((g["width"].as_i64()?, g["height"].as_i64()?))
}

/// Within what a client-side title bar adds (the content is `size`; the
/// surface is the content plus the title bar, at most 60 px).
fn holds(surface: (i64, i64), size: (i32, i32), framed: bool) -> bool {
  let (w, h) = (i64::from(size.0), i64::from(size.1));
  if framed {
    surface.0 == w && surface.1 >= h && surface.1 - h <= 60
  } else {
    surface == (w, h)
  }
}

async fn one_window(
  events: &WindowEvents,
  label: &str,
  size: (i32, i32),
  options: WindowOptions,
  round: u32,
) {
  let framed = !options.frameless;
  let from = events.count();
  let win = Window::new_with_options(size.0, size.1, options)
    .title(&format!("native-e2e-initial-size-{label}-{round}"));
  win.navigate(&format!(
    "laufey-e2e://app/titled/native-e2e-initial-size-{label}-{round}"
  ));
  let mapped = {
    let mut found = None;
    for _ in 0..100 {
      found = events.new_since(from);
      if found.is_some() {
        break;
      }
      tokio::time::sleep(Duration::from_millis(50)).await;
    }
    found
  };
  let Some(mapped) = mapped else {
    check(
      &format!("{label} #{round}: the compositor maps the window"),
      false,
    );
    win.close();
    return;
  };
  let first = geometry(&mapped);
  eprintln!(
    "[e2e] INFO {label} #{round} first mapped at {first:?}, asked {size:?}"
  );
  check(
    &format!(
      "{label} #{round}: the compositor first maps it at the size asked for"
    ),
    first.is_some_and(|g| holds(g, size, framed)),
  );

  // And it stays there: the app's own size and the compositor's agree.
  let id = mapped["id"].as_u64().unwrap_or(0);
  let settled = wait_for(
    || {
      win.get_size() == size
        && tree_node(id)
          .and_then(|n| geometry(&n))
          .is_some_and(|g| holds(g, size, framed))
    },
    30,
    100,
  )
  .await;
  let last = tree_node(id).and_then(|n| geometry(&n));
  eprintln!(
    "[e2e] INFO {label} #{round} then {last:?}, get_size {:?}",
    win.get_size()
  );
  check(
    &format!("{label} #{round}: a moment later it is still that size"),
    settled,
  );
  win.close();
  tokio::time::sleep(Duration::from_millis(300)).await;
}

pub async fn run() {
  if std::env::var_os("SWAYSOCK").is_none() {
    na("a window's first size, from the compositor (no sway: $SWAYSOCK unset)");
    return;
  }
  let Some(events) = WindowEvents::start() else {
    na("a window's first size, from the compositor (swaymsg didn't start)");
    return;
  };
  // Let the subscription be in place before the first window.
  tokio::time::sleep(Duration::from_millis(300)).await;
  laufey::set_quit_on_last_window_closed(false);
  let rounds = std::env::var("LAUFEY_E2E_INITIAL_SIZE_ROUNDS")
    .ok()
    .and_then(|v| v.parse::<u32>().ok())
    .unwrap_or(3)
    .max(1);
  for round in 1..=rounds {
    // A tray-style panel, as an app's first window often is.
    one_window(
      &events,
      "panel",
      (240, 160),
      WindowOptions {
        frameless: true,
        no_activate: true,
        ..Default::default()
      },
      round,
    )
    .await;
    one_window(
      &events,
      "framed",
      (640, 420),
      WindowOptions::default(),
      round,
    )
    .await;
  }
}
