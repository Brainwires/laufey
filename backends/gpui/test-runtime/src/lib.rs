//! The stub runtime of `laufey_gpui --self-test`: a `laufey::main!` library like any other
//! runtime, with no JavaScript engine. It creates a window, drives a native tree through the
//! real C ABI (`laufey::NativeTree`), injects input through the backend's test hook
//! (`test_inject_input`), and checks the events that come back and the tree the host ends
//! up with (`native_tree_snapshot`).
//!
//! It prints one line, `b1-result status=pass|fail ...`, and ends the app with exit code 0
//! or 1. *Hard* checks fail the run. *Soft* checks are reported (`soft_failed=`) without
//! failing it: they depend on the window being the active one, which a CI desktop does not
//! promise (keyboard focus goes to the active window only).

use std::collections::VecDeque;
use std::sync::mpsc::{Receiver, channel};
use std::time::{Duration, Instant};

use laufey::{NativeTree, NativeTreeError, TestInput, Window};
use laufey_ntb1::ids::enums;
use laufey_ntb1::{BatchBuilder, Kind, Prop, StyleProp, Value, listen};
use serde_json::Value as Json;

const WINDOW_SIZE: (i32, i32) = (480, 420);
const PRESSES: u32 = 20;
/// How long one expected event or state may take.
const STEP_TIMEOUT: Duration = Duration::from_secs(10);

// The tree's node ids.
const ROOT: u32 = 1;
const TITLE: u32 = 2;
const TITLE_TEXT: u32 = 3;
const BUTTON: u32 = 4;
const BUTTON_LABEL: u32 = 5;
const BUTTON_TEXT: u32 = 6;
const SCROLL: u32 = 7;
const SCROLL_CONTENT: u32 = 8;
const INPUT: u32 = 9;
const PICTURE: u32 = 10;
const BROKEN_PICTURE: u32 = 11;
const EMPHASIS: u32 = 12;
const EMPHASIS_TEXT: u32 = 13;

const IMAGE: u32 = 1;
const BROKEN_IMAGE: u32 = 2;

/// A 2x2 opaque blue PNG.
const PNG: &[u8] = &[
  0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49,
  0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06,
  0x00, 0x00, 0x00, 0x72, 0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x11, 0x49, 0x44,
  0x41, 0x54, 0x78, 0xda, 0x63, 0xd0, 0xcd, 0xb9, 0xff, 0x1f, 0x84, 0x19, 0x60,
  0x0c, 0x00, 0x52, 0xfc, 0x09, 0xdd, 0x4b, 0x6d, 0x9a, 0x7f, 0x00, 0x00, 0x00,
  0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
];

/// The events the host sent, in arrival order.
struct Events {
  incoming: Receiver<(u32, String)>,
  /// Received and not yet taken by a check.
  pending: VecDeque<(u64, Json)>,
  received: u64,
  window: u32,
}

impl Events {
  /// The first event (pending or still to come) that `matches`, with its arrival number.
  fn wait(
    &mut self,
    timeout: Duration,
    matches: impl Fn(&Json) -> bool,
  ) -> Option<(u64, Json)> {
    let deadline = Instant::now() + timeout;
    let mut checked = 0;
    loop {
      // Everything that has arrived, first: a zero timeout asks about now.
      while let Ok(event) = self.incoming.try_recv() {
        self.take(event);
      }
      while checked < self.pending.len() {
        if matches(&self.pending[checked].1) {
          return self.pending.remove(checked);
        }
        checked += 1;
      }
      let left = deadline.checked_duration_since(Instant::now())?;
      if left.is_zero() {
        return None;
      }
      let event = self.incoming.recv_timeout(left).ok()?;
      self.take(event);
    }
  }

  fn take(&mut self, (window, json): (u32, String)) {
    self.received += 1;
    if window != self.window {
      return;
    }
    // An event that is not JSON is kept as a string, and so matches nothing.
    let event = serde_json::from_str(&json).unwrap_or(Json::String(json));
    self.pending.push_back((self.received, event));
  }

  fn of(
    &mut self,
    node: u32,
    kind: &str,
    timeout: Duration,
  ) -> Option<(u64, Json)> {
    self.wait(timeout, |event| {
      event["node"] == node && event["type"] == kind
    })
  }
}

/// The checks' outcomes.
#[derive(Default)]
struct Report {
  passed: u32,
  failed: Vec<String>,
  soft_failed: Vec<String>,
}

impl Report {
  fn check(&mut self, name: &str, ok: bool) -> bool {
    if ok {
      self.passed += 1;
    } else {
      eprintln!("b1 self-test: FAILED {name}");
      self.failed.push(name.to_string());
    }
    ok
  }

  fn soft(&mut self, name: &str, ok: bool) -> bool {
    if ok {
      self.passed += 1;
    } else {
      eprintln!("b1 self-test: soft check failed: {name}");
      self.soft_failed.push(name.to_string());
    }
    ok
  }
}

fn px(v: f32) -> Value {
  Value::F32(v)
}

/// The first batch: the whole tree.
fn first_batch() -> Vec<u8> {
  let mut b = BatchBuilder::new(1);
  b.create(ROOT, Kind::View)
    .set_style(
      ROOT,
      &[
        (StyleProp::Flex, px(1.0)),
        (
          StyleProp::AlignItems,
          Value::Enum(enums::align_items::FLEX_START),
        ),
        (StyleProp::PaddingTop, px(24.0)),
        (StyleProp::PaddingRight, px(24.0)),
        (StyleProp::PaddingBottom, px(24.0)),
        (StyleProp::PaddingLeft, px(24.0)),
        (StyleProp::Gap, px(16.0)),
        (StyleProp::BackgroundColor, Value::Color(0x101418ff)),
      ],
    )
    .listen(ROOT, listen::LAYOUT)
    // A title with a nested, emphasized run.
    .create(TITLE, Kind::Text)
    .set_style(
      TITLE,
      &[
        (StyleProp::Color, Value::Color(0xe6e6e6ff)),
        (StyleProp::FontSize, px(22.0)),
      ],
    )
    .create(TITLE_TEXT, Kind::RawText)
    .set_text(TITLE_TEXT, "Laufey GPUI ")
    .create(EMPHASIS, Kind::Text)
    .set_style(
      EMPHASIS,
      &[
        (StyleProp::FontWeight, px(700.0)),
        (StyleProp::Color, Value::Color(0x7fb2ffff)),
      ],
    )
    .create(EMPHASIS_TEXT, Kind::RawText)
    .set_text(EMPHASIS_TEXT, "B1")
    .insert(EMPHASIS, EMPHASIS_TEXT, 0)
    .insert(TITLE, TITLE_TEXT, 0)
    .insert(TITLE, EMPHASIS, 0)
    .insert(ROOT, TITLE, 0)
    // A pressable counter.
    .create(BUTTON, Kind::Pressable)
    .set_style(
      BUTTON,
      &[
        (StyleProp::PaddingTop, px(12.0)),
        (StyleProp::PaddingRight, px(12.0)),
        (StyleProp::PaddingBottom, px(12.0)),
        (StyleProp::PaddingLeft, px(12.0)),
        (StyleProp::BackgroundColor, Value::Color(0x2d6cdfff)),
        (StyleProp::BorderTopLeftRadius, px(6.0)),
        (StyleProp::BorderTopRightRadius, px(6.0)),
        (StyleProp::BorderBottomRightRadius, px(6.0)),
        (StyleProp::BorderBottomLeftRadius, px(6.0)),
      ],
    )
    .set_prop(BUTTON, Prop::TestId, &Value::String("counter".into()))
    .listen(BUTTON, listen::PRESS | listen::LAYOUT)
    .create(BUTTON_LABEL, Kind::Text)
    .set_style(
      BUTTON_LABEL,
      &[(StyleProp::Color, Value::Color(0xffffffff))],
    )
    .create(BUTTON_TEXT, Kind::RawText)
    .set_text(BUTTON_TEXT, "Count: 0")
    .insert(BUTTON_LABEL, BUTTON_TEXT, 0)
    .insert(BUTTON, BUTTON_LABEL, 0)
    .insert(ROOT, BUTTON, 0)
    // A scroll view whose content is taller than it.
    .create(SCROLL, Kind::ScrollView)
    .set_style(
      SCROLL,
      &[
        (StyleProp::Width, px(200.0)),
        (StyleProp::Height, px(80.0)),
        (StyleProp::BackgroundColor, Value::Color(0x1c2530ff)),
      ],
    )
    .listen(SCROLL, listen::SCROLL)
    .create(SCROLL_CONTENT, Kind::View)
    .set_style(
      SCROLL_CONTENT,
      &[
        (StyleProp::Height, px(400.0)),
        (StyleProp::BackgroundColor, Value::Color(0x33415580)),
      ],
    )
    .insert(SCROLL, SCROLL_CONTENT, 0)
    .insert(ROOT, SCROLL, 0)
    // A text input with a value.
    .create(INPUT, Kind::TextInput)
    .set_style(
      INPUT,
      &[
        (StyleProp::Width, px(200.0)),
        (StyleProp::PaddingTop, px(6.0)),
        (StyleProp::PaddingRight, px(6.0)),
        (StyleProp::PaddingBottom, px(6.0)),
        (StyleProp::PaddingLeft, px(6.0)),
        (StyleProp::BackgroundColor, Value::Color(0xffffffff)),
      ],
    )
    .set_prop(INPUT, Prop::Placeholder, &Value::String("Type here".into()))
    .set_prop(INPUT, Prop::EventCount, &Value::F64(0.0))
    .set_prop(INPUT, Prop::Value, &Value::String("hello".into()))
    .listen(INPUT, listen::TEXT | listen::FOCUS | listen::KEY)
    .insert(ROOT, INPUT, 0)
    // Two images: one that decodes and one that does not.
    .image_data(IMAGE, "image/png", PNG)
    .image_data(BROKEN_IMAGE, "image/png", b"not a png")
    .create(PICTURE, Kind::Image)
    .set_style(
      PICTURE,
      &[(StyleProp::Width, px(32.0)), (StyleProp::Height, px(32.0))],
    )
    .set_prop(
      PICTURE,
      Prop::ResizeMode,
      &Value::Enum(enums::resize_mode::STRETCH),
    )
    .listen(PICTURE, listen::IMAGE)
    .set_image(PICTURE, IMAGE)
    .insert(ROOT, PICTURE, 0)
    .create(BROKEN_PICTURE, Kind::Image)
    .set_style(
      BROKEN_PICTURE,
      &[(StyleProp::Width, px(8.0)), (StyleProp::Height, px(8.0))],
    )
    .listen(BROKEN_PICTURE, listen::IMAGE)
    .set_image(BROKEN_PICTURE, BROKEN_IMAGE)
    .insert(ROOT, BROKEN_PICTURE, 0)
    .set_root(ROOT);
  b.finish()
}

fn snapshot(window: &Window) -> Option<Json> {
  serde_json::from_str(&NativeTree::snapshot(window)?).ok()
}

fn node<'a>(snapshot: &'a Json, id: u32) -> Option<&'a Json> {
  snapshot["nodes"]
    .as_array()?
    .iter()
    .find(|node| node["id"] == id)
}

/// A node's bounds in the window: x, y, width, height.
fn bounds(snapshot: &Json, id: u32) -> Option<[f64; 4]> {
  let bounds = node(snapshot, id)?["bounds"].as_array()?;
  Some([
    bounds.first()?.as_f64()?,
    bounds.get(1)?.as_f64()?,
    bounds.get(2)?.as_f64()?,
    bounds.get(3)?.as_f64()?,
  ])
}

fn center(bounds: [f64; 4]) -> (f64, f64) {
  (bounds[0] + bounds[2] / 2.0, bounds[1] + bounds[3] / 2.0)
}

/// Wait until the window has drawn the tree and its layout has stopped changing (the window
/// is created at a default size and resized).
fn settled_snapshot(window: &Window) -> Option<Json> {
  let deadline = Instant::now() + STEP_TIMEOUT;
  let mut previous: Option<Json> = None;
  while Instant::now() < deadline {
    std::thread::sleep(Duration::from_millis(150));
    let Some(current) = snapshot(window) else {
      continue;
    };
    let drawn = bounds(&current, ROOT)
      .is_some_and(|b| b[2] > 0.0 && b[3] > 0.0)
      && bounds(&current, BUTTON).is_some()
      && bounds(&current, SCROLL).is_some();
    if drawn && previous.as_ref() == Some(&current) {
      return Some(current);
    }
    previous = Some(current);
  }
  None
}

fn click(window: &Window, at: (f64, f64)) -> bool {
  let button = |pressed| TestInput::MouseButton {
    button: 0,
    pressed,
    x: at.0,
    y: at.1,
    modifiers: 0,
  };
  let moved = TestInput::MouseMove {
    x: at.0,
    y: at.1,
    modifiers: 0,
  };
  laufey::test_inject_input(window.id(), &moved)
    && laufey::test_inject_input(window.id(), &button(true))
    && laufey::test_inject_input(window.id(), &button(false))
}

fn close_to(a: f64, b: f64) -> bool {
  (a - b).abs() <= 1.0
}

fn run(report: &mut Report) -> Vec<(&'static str, String)> {
  let mut stats = Vec::new();
  let started = Instant::now();

  // --- The capability and the unsupported rest ---
  let capability = laufey::window_capabilities().native_tree();
  report.check(
    "capability",
    capability.map(|c| c.version) == Some(u32::from(laufey_ntb1::VERSION))
      && NativeTree::version() == Some(1),
  );
  // Entries this backend leaves NULL answer "unsupported" without a crash.
  report.check(
    "unsupported-entries",
    laufey::read_clipboard_text().is_none()
      && laufey::menu_snapshot(laufey::MenuKind::Dock).is_none()
      && !laufey::window_capabilities().screens(),
  );

  let (sender, incoming) = channel();
  report.check(
    "event-handler",
    NativeTree::on_event(move |window, json| {
      let _ = sender.send((window, json.to_string()));
    }),
  );

  // --- A window and its tree ---
  let window =
    Window::new(WINDOW_SIZE.0, WINDOW_SIZE.1).title("laufey gpui self-test");
  if !report.check("window", window.id() != 0) {
    return stats;
  }
  let mut events = Events {
    incoming,
    pending: VecDeque::new(),
    received: 0,
    window: window.id(),
  };
  report.check(
    "commit",
    NativeTree::commit(&window, &first_batch()) == Ok(()),
  );

  let Some(tree) = settled_snapshot(&window) else {
    report.check("first-frame", false);
    return stats;
  };
  report.check("first-frame", true);
  stats.push(("first_frame_ms", started.elapsed().as_millis().to_string()));
  report.check(
    "tree",
    tree["root"] == ROOT
      && tree["seq"] == 1
      && tree["nodes"]
        .as_array()
        .is_some_and(|nodes| nodes.len() == 13)
      && node(&tree, ROOT).is_some_and(|root| {
        root["children"]
          == serde_json::json!([
            TITLE,
            BUTTON,
            SCROLL,
            INPUT,
            PICTURE,
            BROKEN_PICTURE
          ])
      })
      && node(&tree, BUTTON_TEXT)
        .is_some_and(|text| text["text"] == "Count: 0" && text["parent"] == 5)
      && node(&tree, INPUT).is_some_and(|input| input["text"] == "hello")
      && node(&tree, BUTTON).is_some_and(|b| b["kind"] == "Pressable"),
  );

  // --- Window calls, from this thread and re-entrantly from the UI thread ---
  let size = window.get_size();
  report.check("window-size", size.0 > 0 && size.1 > 0);
  report.soft("window-size-exact", size == WINDOW_SIZE);
  let id = window.id();
  let on_ui = laufey::try_run_on_ui_thread(move || {
    (laufey::is_ui_thread(), Window::from_id(id).get_size())
  });
  report.check(
    "ui-thread-task",
    !laufey::is_ui_thread() && on_ui == Ok((true, size)),
  );

  // --- Layout ---
  let root_bounds = bounds(&tree, ROOT).unwrap_or_default();
  let button_bounds = bounds(&tree, BUTTON).unwrap_or_default();
  report.check(
    "root-fills-window",
    close_to(root_bounds[2], f64::from(size.0))
      && close_to(root_bounds[3], f64::from(size.1)),
  );
  // The button sits inside the root's 24px padding, below the title, and is at least its
  // own padding in size (its text adds to that where the platform has a font for it).
  report.check(
    "button-placed",
    close_to(button_bounds[0], 24.0)
      && button_bounds[1] >= 24.0
      && button_bounds[2] >= 24.0
      && button_bounds[3] >= 24.0,
  );
  // The last `layout` of each listening node agrees with the frame.
  let last_layout = |events: &mut Events, node: u32| {
    let mut last = None;
    while let Some((_, event)) = events.of(node, "layout", Duration::ZERO) {
      last = Some(event);
    }
    last
  };
  let root_layout = last_layout(&mut events, ROOT);
  let button_layout = last_layout(&mut events, BUTTON);
  report.check(
    "layout-events",
    root_layout.is_some_and(|l| {
      l["x"] == 0.0
        && l["y"] == 0.0
        && close_to(l["width"].as_f64().unwrap_or(-1.0), root_bounds[2])
        && close_to(l["height"].as_f64().unwrap_or(-1.0), root_bounds[3])
    }) && button_layout.is_some_and(|l| {
      close_to(l["x"].as_f64().unwrap_or(-1.0), button_bounds[0])
        && close_to(l["y"].as_f64().unwrap_or(-1.0), button_bounds[1])
        && close_to(l["width"].as_f64().unwrap_or(-1.0), button_bounds[2])
        && close_to(l["height"].as_f64().unwrap_or(-1.0), button_bounds[3])
    }),
  );

  // --- Images ---
  report.check(
    "image-load",
    events
      .of(PICTURE, "imageLoad", STEP_TIMEOUT)
      .is_some_and(|(_, event)| event["image_id"] == IMAGE),
  );
  report.check(
    "image-error",
    events
      .of(BROKEN_PICTURE, "imageError", STEP_TIMEOUT)
      .is_some_and(|(_, event)| event["image_id"] == BROKEN_IMAGE),
  );

  // --- Batches that must be refused ---
  report.check(
    "malformed-batch",
    NativeTree::commit(&window, b"junk") == Err(NativeTreeError::Malformed)
      && NativeTree::commit(&window, &first_batch()[..40])
        == Err(NativeTreeError::Malformed),
  );
  report.check(
    "unknown-window",
    NativeTree::commit(&Window::from_id(9999), &first_batch())
      == Err(NativeTreeError::NoWindow),
  );
  // Decodes, but names a node that does not exist: refused whole on the UI thread, with
  // the good operation before the fault undone.
  let mut bad = BatchBuilder::new(77);
  bad
    .set_text(BUTTON_TEXT, "must not show")
    .insert(ROOT, 999, 0);
  report.check(
    "batch-error",
    NativeTree::commit(&window, &bad.finish()) == Ok(())
      && events
        .of(0, "batchError", STEP_TIMEOUT)
        .is_some_and(|(_, event)| {
          event["seq"] == 77 && event["error"].is_string()
        }),
  );
  report.check(
    "rejected-batch-changed-nothing",
    snapshot(&window).is_some_and(|after| {
      after["seq"] == 1
        && node(&after, BUTTON_TEXT)
          .is_some_and(|text| text["text"] == "Count: 0")
    }),
  );

  // --- Presses: input in, events out, a batch back, the tree read back ---
  let target = center(button_bounds);
  let mut presses_ok = true;
  let mut round_trips = Vec::new();
  for count in 1..=PRESSES {
    let pressed_at = Instant::now();
    if !click(&window, target) {
      eprintln!("b1 self-test: press {count}: the input was not taken");
      presses_ok = false;
      break;
    }
    let press_in = events.of(BUTTON, "pressIn", STEP_TIMEOUT);
    let press_out = events.of(BUTTON, "pressOut", STEP_TIMEOUT);
    let press = events.of(BUTTON, "press", STEP_TIMEOUT);
    let (Some(press_in), Some(press_out), Some(press)) =
      (press_in, press_out, press)
    else {
      eprintln!("b1 self-test: press {count}: an event is missing");
      presses_ok = false;
      break;
    };
    // In order, and at the point pressed, in the button's own coordinates.
    let local = (target.0 - button_bounds[0], target.1 - button_bounds[1]);
    let at_point = |event: &Json| {
      close_to(event["x"].as_f64().unwrap_or(-1.0), local.0)
        && close_to(event["y"].as_f64().unwrap_or(-1.0), local.1)
    };
    if !(press_in.0 < press_out.0
      && press_out.0 < press.0
      && at_point(&press_in.1)
      && at_point(&press.1))
    {
      eprintln!("b1 self-test: press {count}: wrong order or position");
      presses_ok = false;
      break;
    }
    // What a renderer does with the press: commit the new label.
    let mut b = BatchBuilder::new(100 + count);
    b.set_text(BUTTON_TEXT, &format!("Count: {count}"));
    if NativeTree::commit(&window, &b.finish()) != Ok(()) {
      presses_ok = false;
      break;
    }
    // The snapshot queues behind the batch, so it sees it applied.
    let label = snapshot(&window).and_then(|tree| {
      node(&tree, BUTTON_TEXT).map(|text| text["text"].clone())
    });
    round_trips.push(pressed_at.elapsed());
    if label != Some(Json::String(format!("Count: {count}"))) {
      eprintln!("b1 self-test: press {count}: the label is {label:?}");
      presses_ok = false;
      break;
    }
  }
  report.check("presses", presses_ok);
  stats.push(("presses", round_trips.len().to_string()));
  if !round_trips.is_empty() {
    let average = round_trips.iter().map(Duration::as_secs_f64).sum::<f64>()
      * 1e3
      / round_trips.len() as f64;
    stats.push(("round_trip_ms_avg", format!("{average:.3}")));
  }
  // No press was reported twice, and nothing reached a node that does not listen.
  report.check(
    "no-stray-press",
    events
      .wait(Duration::from_millis(100), |event| {
        event["type"] == "press" || event["type"] == "longPress"
      })
      .is_none(),
  );

  // --- A disabled pressable hears nothing ---
  // The batch also widens the button: its `layout` event says a frame has been drawn from
  // the new tree, without the press listeners.
  let mut b = BatchBuilder::new(200);
  b.set_prop(BUTTON, Prop::Disabled, &Value::Bool(true))
    .set_style(BUTTON, &[(StyleProp::Width, px(220.0))]);
  let disabled = NativeTree::commit(&window, &b.finish()) == Ok(())
    && events
      .wait(STEP_TIMEOUT, |event| {
        event["node"] == BUTTON
          && event["type"] == "layout"
          && event["width"] == 220.0
      })
      .is_some()
    && click(&window, target)
    && events
      .of(BUTTON, "pressIn", Duration::from_millis(300))
      .is_none();
  report.check("disabled-pressable", disabled);

  // --- Scrolling: one coalesced event per frame, in the view's coordinates ---
  let scroll_bounds = bounds(&tree, SCROLL).unwrap_or_default();
  let over_scroll = center(scroll_bounds);
  let moved = TestInput::MouseMove {
    x: over_scroll.0,
    y: over_scroll.1,
    modifiers: 0,
  };
  let wheel = TestInput::Wheel {
    delta_x: 0.0,
    delta_y: 30.0,
    delta_mode: 0,
    x: over_scroll.0,
    y: over_scroll.1,
    modifiers: 0,
  };
  let scrolled = laufey::test_inject_input(window.id(), &moved)
    && laufey::test_inject_input(window.id(), &wheel)
    && laufey::test_inject_input(window.id(), &wheel);
  let scroll_event = events.of(SCROLL, "scroll", STEP_TIMEOUT);
  report.check(
    "scroll",
    scrolled
      && scroll_event.is_some_and(|(_, event)| {
        event["y"]
          .as_f64()
          .is_some_and(|y| (0.5..=60.5).contains(&y))
          && event["x"] == 0.0
          && close_to(event["contentHeight"].as_f64().unwrap_or(-1.0), 400.0)
      }),
  );

  // --- The text input: focus and typing need the active window (soft) ---
  let input_bounds = bounds(&tree, INPUT).unwrap_or_default();
  let focused = click(&window, center(input_bounds))
    && events.of(INPUT, "focus", Duration::from_secs(2)).is_some();
  report.soft("input-focus", focused);
  if focused {
    let key = |pressed| TestInput::Key {
      key: "!".into(),
      code: "Digit1".into(),
      pressed,
      repeat: false,
      modifiers: 0,
    };
    let typed = laufey::test_inject_input(window.id(), &key(true))
      && laufey::test_inject_input(window.id(), &key(false));
    report.soft(
      "input-key",
      typed
        && events
          .of(INPUT, "key", Duration::from_secs(2))
          .is_some_and(|(_, event)| event["key"] == "!"),
    );
  }
  // A value from the runtime with a current event count replaces the text.
  let mut b = BatchBuilder::new(300);
  b.set_prop(INPUT, Prop::EventCount, &Value::F64(1e6))
    .set_prop(
      INPUT,
      Prop::Value,
      &Value::String("from the runtime".into()),
    );
  report.check(
    "input-value",
    NativeTree::commit(&window, &b.finish()) == Ok(())
      && snapshot(&window).is_some_and(|tree| {
        node(&tree, INPUT)
          .is_some_and(|input| input["text"] == "from the runtime")
      }),
  );

  // --- Moving and destroying ---
  let mut b = BatchBuilder::new(400);
  b.insert(ROOT, SCROLL, TITLE)
    .remove(ROOT, PICTURE)
    .destroy(PICTURE)
    .destroy(BUTTON)
    .image_free(IMAGE)
    .image_free(BROKEN_IMAGE);
  report.check(
    "move-and-destroy",
    NativeTree::commit(&window, &b.finish()) == Ok(())
      && snapshot(&window).is_some_and(|tree| {
        tree["seq"] == 400
          && tree["nodes"]
            .as_array()
            .is_some_and(|nodes| nodes.len() == 9)
          && node(&tree, BUTTON_TEXT).is_none()
          && node(&tree, ROOT).is_some_and(|root| {
            root["children"]
              == serde_json::json!([SCROLL, TITLE, INPUT, BROKEN_PICTURE])
          })
      }),
  );
  stats.push(("events", events.received.to_string()));
  stats.push(("total_ms", started.elapsed().as_millis().to_string()));
  stats
}

laufey::main!(|| {
  let mut report = Report::default();
  let stats =
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| run(&mut report)))
      .unwrap_or_else(|_| {
        report.failed.push("panic".into());
        Vec::new()
      });
  let passed = report.failed.is_empty();
  let mut line = format!(
    "b1-result status={} checks={} failed={} soft_failed={}",
    if passed { "pass" } else { "fail" },
    report.passed,
    if report.failed.is_empty() {
      "none".to_string()
    } else {
      report.failed.join(",")
    },
    if report.soft_failed.is_empty() {
      "none".to_string()
    } else {
      report.soft_failed.join(",")
    },
  );
  for (key, value) in stats {
    line.push_str(&format!(" {key}={value}"));
  }
  println!("{line}");
  let code = if passed { 0 } else { 1 };
  // The host ends the process; without it (a backend with no exit_app) this thread does.
  if !laufey::exit(code) {
    std::process::exit(code);
  }
});
