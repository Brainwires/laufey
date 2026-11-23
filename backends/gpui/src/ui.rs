//! The window: one view that holds the last committed tree and draws it with GPUI elements.
//!
//! Commits arrive from the JS thread on an async channel; a foreground task awaits it and
//! notifies the view, so nothing polls. A pressable's `on_click` sends a press event to JS.
//!
//! The self-test (`--self-test`) drives the same event channel instead of the mouse: after
//! each tree has been on screen for a frame it sends the next press, and it checks the counter
//! text of every tree that comes back.

use std::rc::Rc;
use std::time::{Duration, Instant};

use futures::StreamExt as _;
use futures::channel::mpsc::UnboundedReceiver;
use gpui::{
  AnyElement, App, Bounds, Context, Div, IntoElement, Render, SharedString,
  Window, WindowBounds, WindowOptions, div, prelude::*, px, rgb, size,
};

use crate::js::{Commit, JsHost};
use crate::tree::{
  Align, FlexDirection, Node, NodeType, Style, UiEvent, parse_color,
};

/// How long the self-test waits for all its presses to come back.
const SELF_TEST_TIMEOUT: Duration = Duration::from_secs(30);

pub struct SelfTest {
  presses: u32,
  sent: u32,
  press_sent_at: Option<Instant>,
  /// Press sent -> the recommitted tree received on the UI thread.
  to_commit: Vec<Duration>,
  /// Press sent -> the frame after the one that drew the recommitted tree.
  to_frame: Vec<Duration>,
}

impl SelfTest {
  pub fn new(presses: u32) -> Self {
    Self {
      presses,
      sent: 0,
      press_sent_at: None,
      to_commit: Vec::new(),
      to_frame: Vec::new(),
    }
  }
}

pub struct SpikeView {
  js: Rc<JsHost>,
  launched_at: Instant,
  tree: Option<Node>,
  /// The tree changed since the last frame callback was scheduled.
  undrawn: bool,
  first_commit_at: Option<Instant>,
  first_frame_at: Option<Instant>,
  self_test: Option<SelfTest>,
}

impl SpikeView {
  fn new(
    js: Rc<JsHost>,
    mut commits: UnboundedReceiver<Commit>,
    launched_at: Instant,
    self_test: Option<SelfTest>,
    cx: &mut Context<Self>,
  ) -> Self {
    cx.spawn(async move |this, cx| {
      while let Some(commit) = commits.next().await {
        if this.update(cx, |view, cx| view.apply(commit, cx)).is_err() {
          break;
        }
      }
    })
    .detach();
    if self_test.is_some() {
      cx.spawn(async move |this, cx| {
        cx.background_executor().timer(SELF_TEST_TIMEOUT).await;
        let _ = this.update(cx, |view, _| {
          view.finish(Err(format!("timed out after {SELF_TEST_TIMEOUT:?}")))
        });
      })
      .detach();
    }
    Self {
      js,
      launched_at,
      tree: None,
      undrawn: false,
      first_commit_at: None,
      first_frame_at: None,
      self_test,
    }
  }

  fn apply(&mut self, commit: Commit, cx: &mut Context<Self>) {
    let now = Instant::now();
    self.first_commit_at.get_or_insert(now);
    if let Some(test) = &mut self.self_test
      && let Some(sent_at) = test.press_sent_at
    {
      test.to_commit.push(now.duration_since(sent_at));
    }
    self.tree = Some(commit);
    self.undrawn = true;
    cx.notify();
  }

  /// Runs at the start of the frame after the one that drew the current tree.
  fn drawn(&mut self) {
    let now = Instant::now();
    if self.first_frame_at.is_none() {
      self.first_frame_at = Some(now);
      println!(
        "b0-window first-frame ms={}",
        now.duration_since(self.launched_at).as_millis()
      );
    }
    if self.self_test.is_some() {
      self.self_test_step(now);
    }
  }

  fn self_test_step(&mut self, now: Instant) {
    let outcome = self.self_test_check(now);
    if let Some(outcome) = outcome {
      self.finish(outcome);
    }
  }

  /// Check the tree on screen against the presses sent so far and send the next press.
  /// Returns the test's outcome once it has one.
  fn self_test_check(&mut self, now: Instant) -> Option<Result<u32, String>> {
    let (Some(test), Some(tree)) = (&mut self.self_test, &self.tree) else {
      return None;
    };
    let Some(button) = tree.find(NodeType::Pressable) else {
      return Some(Err("the tree has no pressable".into()));
    };
    let (id, label) = (button.id, button.text_content());
    let expected = format!("Count: {}", test.sent);
    if label != expected {
      return Some(Err(format!(
        "after {} presses the button says {label:?}, expected {expected:?}",
        test.sent
      )));
    }
    if let Some(sent_at) = test.press_sent_at.take() {
      test.to_frame.push(now.duration_since(sent_at));
    }
    if test.sent == test.presses {
      return Some(Ok(test.sent));
    }
    test.sent += 1;
    test.press_sent_at = Some(Instant::now());
    if !self.js.send(UiEvent::Press { id }) {
      return Some(Err("the js thread is gone".into()));
    }
    None
  }

  /// Print the result line, stop the JS thread and exit.
  fn finish(&mut self, outcome: Result<u32, String>) {
    let Some(test) = self.self_test.take() else {
      return;
    };
    let since_launch = |at: Option<Instant>| {
      at.map(|at| at.duration_since(self.launched_at).as_secs_f64() * 1e3)
        .unwrap_or(f64::NAN)
    };
    let average = |samples: &[Duration]| {
      if samples.is_empty() {
        return f64::NAN;
      }
      samples.iter().map(Duration::as_secs_f64).sum::<f64>() * 1e3
        / samples.len() as f64
    };
    let counter = match &outcome {
      Ok(counter) => counter.to_string(),
      Err(_) => "mismatch".into(),
    };
    println!(
      "b0-result presses={} counter={counter} first_commit_ms={:.1} first_frame_ms={:.1} \
             round_trip_ms_avg={:.3} round_trip_commit_ms_avg={:.3}",
      test.sent,
      since_launch(self.first_commit_at),
      since_launch(self.first_frame_at),
      average(&test.to_frame),
      average(&test.to_commit),
    );
    self.js.shutdown();
    match outcome {
      Ok(_) => std::process::exit(0),
      Err(reason) => {
        eprintln!("b0 self-test failed: {reason}");
        std::process::exit(1);
      }
    }
  }

  fn element(&self, node: &Node) -> AnyElement {
    let base = styled(div(), &node.style);
    let base = match &node.text {
      Some(text) => base.child(SharedString::from(text.clone())),
      None => base,
    };
    let base =
      base.children(node.children.iter().map(|child| self.element(child)));
    match node.kind {
      NodeType::View | NodeType::Text => base.into_any_element(),
      NodeType::Pressable => {
        let (js, id) = (self.js.clone(), node.id);
        base
          .id(node.id as usize)
          .cursor_pointer()
          .on_click(move |_, _, _| {
            js.send(UiEvent::Press { id });
          })
          .into_any_element()
      }
    }
  }
}

/// The React Native style subset, on a GPUI `div`. As in React Native, every node is a flex
/// container and a column unless it says otherwise.
fn styled(mut el: Div, style: &Style) -> Div {
  el = el.flex();
  el = match style.flex_direction {
    Some(FlexDirection::Row) => el.flex_row(),
    Some(FlexDirection::Column) | None => el.flex_col(),
  };
  if let Some(flex) = style.flex
    && flex > 0.0
  {
    el = el.flex_grow(flex).flex_shrink(1.0).flex_basis(px(0.0));
  }
  el = match style.align_items {
    Some(Align::FlexStart) => el.items_start(),
    Some(Align::Center) => el.items_center(),
    Some(Align::FlexEnd) => el.items_end(),
    None => el,
  };
  el = match style.justify_content {
    Some(Align::FlexStart) => el.justify_start(),
    Some(Align::Center) => el.justify_center(),
    Some(Align::FlexEnd) => el.justify_end(),
    None => el,
  };
  if let Some(padding) = style.padding {
    el = el.p(px(padding));
  }
  if let Some(gap) = style.gap {
    el = el.gap(px(gap));
  }
  if let Some(width) = style.width {
    el = el.w(px(width));
  }
  if let Some(height) = style.height {
    el = el.h(px(height));
  }
  if let Some(color) = style.background_color.as_deref().and_then(parse_color) {
    el = el.bg(rgb(color));
  }
  if let Some(color) = style.color.as_deref().and_then(parse_color) {
    el = el.text_color(rgb(color));
  }
  if let Some(font_size) = style.font_size {
    el = el.text_size(px(font_size));
  }
  if let Some(radius) = style.border_radius {
    el = el.rounded(px(radius));
  }
  el
}

impl Render for SpikeView {
  fn render(
    &mut self,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) -> impl IntoElement {
    if std::mem::take(&mut self.undrawn) {
      cx.on_next_frame(window, |view, _, _| view.drawn());
    }
    let root = div().size_full().flex().flex_col().bg(rgb(0x000000));
    match &self.tree {
      Some(tree) => root.child(self.element(tree)),
      None => root,
    }
  }
}

/// Run the GPUI application on the calling (main) thread. Does not return in the self-test.
pub fn run(
  js: JsHost,
  commits: UnboundedReceiver<Commit>,
  launched_at: Instant,
  self_test: Option<SelfTest>,
) {
  let js = Rc::new(js);
  gpui_platform::application().run(move |cx: &mut App| {
    let bounds = Bounds::centered(None, size(px(480.), px(320.)), cx);
    let view_js = js.clone();
    cx.open_window(
      WindowOptions {
        window_bounds: Some(WindowBounds::Windowed(bounds)),
        ..Default::default()
      },
      move |_, cx| {
        cx.new(|cx| {
          SpikeView::new(view_js, commits, launched_at, self_test, cx)
        })
      },
    )
    .expect("open the spike window");
    cx.on_window_closed(move |cx, _| {
      js.shutdown();
      cx.quit();
    })
    .detach();
    cx.activate(true);
  });
}
