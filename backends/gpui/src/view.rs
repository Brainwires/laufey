//! One window: its retained tree (`laufey_ntb1::Tree`) and the GPUI elements drawn from it.
//!
//! Batches arrive on the UI thread already decoded (`api::native_tree_commit`) and are
//! applied whole by [`TreeView::apply_batch`]; a batch the tree refuses changes nothing and
//! is reported as a `batchError` event. Every frame the tree is mapped onto elements:
//!
//! | Node | Element |
//! | --- | --- |
//! | View, Pressable | `div` |
//! | ScrollView | `div` with a scroll handle |
//! | Text | `div` holding one `StyledText`: its RawText descendants, a run per nested Text |
//! | RawText | (its Text's content) |
//! | Image | `img` over a decoded `RenderImage` |
//! | TextInput | `div` holding the node's [`TextInput`] entity |
//!
//! Every element is wrapped in [`Measured`], which records its bounds during prepaint. Once
//! the frame is painted, [`TreeView::after_paint`] turns the bounds into `layout` events
//! (only for what changed) and the scroll handles' offsets into `scroll` events, one per
//! node per frame.
//!
//! Events never call the runtime from here: they are queued (`host::emit_*`), see `ui.rs`.

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::ops::Range;
use std::rc::Rc;
use std::sync::Arc;
use std::time::Duration;

use gpui::{
  AnyElement, App, AppContext as _, Bounds, Context, Div, Element, ElementId,
  Entity, FocusHandle, GlobalElementId, HighlightStyle, Image, ImageFormat,
  ImageSource, InspectorElementId, InteractiveElement, IntoElement,
  KeyDownEvent, KeyUpEvent, LayoutId, MouseButton, MouseDownEvent,
  MouseMoveEvent, MouseUpEvent, ParentElement, Pixels, Point, Render,
  RenderImage, ScrollDelta, ScrollEvent, ScrollHandle, SharedString,
  StatefulInteractiveElement, Styled, StyledImage, StyledText, Window, canvas,
  div, img,
};
use laufey_ntb1::{
  Applied, Batch, ImageOp, Kind, Node, Prop, Tree, Value, listen,
};
use serde_json::json;

use crate::api::{button_to_laufey, modifiers_to_laufey};
use crate::host::{self, emit_tree_event};
use crate::input::{InputProps, TextInput};
use crate::style;

/// How long a press is held before it is a long press (React Native's default).
const LONG_PRESS: Duration = Duration::from_millis(500);

/// Host state of a node, beside the tree.
#[derive(Default)]
struct NodeState {
  /// A ScrollView's scroll position.
  scroll: Option<ScrollHandle>,
  /// A TextInput's field.
  input: Option<Entity<TextInput>>,
  /// A focus handle, for a node that listens for keys.
  focus: Option<FocusHandle>,
  /// The `layout` last reported: x, y (in the parent), width, height.
  layout: Option<[f32; 4]>,
  /// The scroll offset last reported.
  scrolled: Option<(f32, f32)>,
}

/// An IMAGE_DATA image.
enum ImageState {
  Decoding,
  Ready(Arc<RenderImage>),
  Failed,
}

struct ImageEntry {
  /// Which IMAGE_DATA of this id the state belongs to: a decode that finishes after the
  /// image was freed or replaced is dropped.
  generation: u64,
  state: ImageState,
}

/// The press in progress.
struct Pressed {
  node: u32,
  generation: u64,
  /// The long press fired: the release is not a press.
  long: bool,
}

#[derive(Default)]
struct PressState {
  pressed: RefCell<Option<Pressed>>,
  generations: Cell<u64>,
}

pub struct TreeView {
  window_id: u32,
  tree: Tree,
  nodes: HashMap<u32, NodeState>,
  images: HashMap<u32, ImageEntry>,
  image_generations: u64,
  /// The bounds of every element of the frame being drawn, by node, in window coordinates.
  measured: Rc<RefCell<HashMap<u32, Bounds<Pixels>>>>,
  press: Rc<PressState>,
  /// The window's own focus: keys go here when no node has it.
  focus: FocusHandle,
  shown: bool,
  /// The size and position last reported to the resize and move handlers.
  reported_size: Option<(i32, i32)>,
  reported_position: Option<(i32, i32)>,
  /// A mouse move not yet reported: one per frame.
  pending_move: Rc<Cell<Option<(f64, f64, u32)>>>,
}

fn px_f32(value: Pixels) -> f32 {
  f32::from(value)
}

fn rounded(value: Pixels) -> i32 {
  f32::from(value).round() as i32
}

impl TreeView {
  pub fn new(
    window_id: u32,
    shown: bool,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) -> Self {
    let focus = cx.focus_handle();
    window.focus(&focus, cx);

    cx.observe_window_bounds(window, |this, window, _| {
      this.report_bounds(window)
    })
    .detach();
    cx.observe_window_activation(window, |this, window, _| {
      host::emit_focused(this.window_id, window.is_window_active());
    })
    .detach();
    // With a close-requested handler registered the window stays open and the runtime
    // decides (`close_window`); without one it closes.
    window.on_window_should_close(cx, move |_, _| {
      let handler = host::handlers().close_requested;
      match handler {
        Some(h) => {
          crate::ui::post_call(move || {
            // SAFETY: the runtime's handler.
            unsafe { (h.f)(h.data as *mut std::ffi::c_void, window_id) };
          });
          false
        }
        None => true,
      }
    });

    Self {
      window_id,
      tree: Tree::new(),
      nodes: HashMap::new(),
      images: HashMap::new(),
      image_generations: 0,
      measured: Rc::default(),
      press: Rc::default(),
      focus,
      shown,
      reported_size: None,
      reported_position: None,
      pending_move: Rc::default(),
    }
  }

  pub fn is_shown(&self) -> bool {
    self.shown
  }

  pub fn set_shown(&mut self) {
    self.shown = true;
  }

  fn report_bounds(&mut self, window: &mut Window) {
    let viewport = window.viewport_size();
    let size = (rounded(viewport.width), rounded(viewport.height));
    if self.reported_size != Some(size) {
      // The first report is the window's creation, not a resize.
      if self.reported_size.is_some() {
        host::emit_resize(self.window_id, size.0, size.1);
      }
      self.reported_size = Some(size);
    }
    let origin = window.bounds().origin;
    let position = (rounded(origin.x), rounded(origin.y));
    if self.reported_position != Some(position) {
      if self.reported_position.is_some() {
        host::emit_moved(self.window_id, position.0, position.1);
      }
      self.reported_position = Some(position);
    }
  }

  fn emit(&self, event: serde_json::Value) {
    emit_tree_event(self.window_id, event.to_string());
  }

  // --- Batches --------------------------------------------------------------------------

  /// Apply one batch, whole or not at all.
  pub fn apply_batch(
    &mut self,
    batch: Batch,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) {
    let seq = batch.seq;
    let applied = match self.tree.apply(batch) {
      Ok(applied) => applied,
      Err(error) => {
        eprintln!(
          "laufey_gpui: window {}: batch {seq} rejected: {error}",
          self.window_id
        );
        self.emit(json!({
          "node": 0,
          "type": "batchError",
          "seq": seq,
          "error": error.to_string(),
        }));
        return;
      }
    };
    let Applied {
      created,
      destroyed,
      props_changed,
      images_set,
      images,
    } = applied;
    for id in destroyed {
      self.nodes.remove(&id);
    }
    for id in created {
      // (A node the same batch destroyed is not in the tree any more.)
      let Some(kind) = self.tree.get(id).map(|node| node.kind) else {
        continue;
      };
      let state = self.nodes.entry(id).or_default();
      match kind {
        Kind::ScrollView => state.scroll = Some(ScrollHandle::new()),
        Kind::TextInput => {
          let window_id = self.window_id;
          state.input =
            Some(cx.new(|cx| TextInput::new(window_id, id, window, cx)));
        }
        _ => {}
      }
    }
    for op in images {
      match op {
        ImageOp::Data {
          image_id,
          mime,
          bytes,
        } => self.decode_image(image_id, mime, bytes, window, cx),
        ImageOp::Free { image_id } => {
          if let Some(entry) = self.images.remove(&image_id) {
            self.drop_image(entry, window, cx);
          }
        }
      }
    }
    self.sync_inputs(&props_changed, cx);
    // A node pointed at an image that is already decoded (or already failed) hears at once.
    for id in images_set {
      self.report_image(id);
    }
    cx.notify();
  }

  /// Hand every TextInput its props; the ones in `value_set` may also take a new value.
  fn sync_inputs(&mut self, value_set: &[u32], cx: &mut Context<Self>) {
    for (id, state) in &self.nodes {
      let (Some(input), Some(node)) = (&state.input, self.tree.get(*id)) else {
        continue;
      };
      let props = InputProps {
        placeholder: node
          .prop(Prop::Placeholder)
          .and_then(Value::as_str)
          .map(|s| SharedString::from(s.to_owned()))
          .unwrap_or_default(),
        placeholder_color: node
          .prop(Prop::PlaceholderTextColor)
          .and_then(Value::as_color)
          .map(style::color),
        editable: node
          .prop(Prop::Editable)
          .and_then(Value::as_bool)
          .unwrap_or(true),
        secure: node
          .prop(Prop::SecureTextEntry)
          .and_then(Value::as_bool)
          .unwrap_or(false),
        listen: node.listen,
      };
      let value = value_set
        .contains(id)
        .then(|| {
          let event_count = node
            .prop(Prop::EventCount)
            .and_then(Value::as_number)
            .map(|count| count.max(0.0) as u64);
          node
            .prop(Prop::Value)
            .and_then(Value::as_str)
            .map(|value| (value, event_count))
        })
        .flatten();
      input.update(cx, |input, cx| input.sync(props, value, cx));
    }
  }

  // --- Images ---------------------------------------------------------------------------

  fn decode_image(
    &mut self,
    image_id: u32,
    mime: String,
    bytes: Vec<u8>,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) {
    self.image_generations += 1;
    let generation = self.image_generations;
    let previous = self.images.insert(
      image_id,
      ImageEntry {
        generation,
        state: ImageState::Decoding,
      },
    );
    if let Some(previous) = previous {
      self.drop_image(previous, window, cx);
    }
    let svg_renderer = cx.svg_renderer();
    // Decoding is off the UI thread; the result comes back to this view.
    let decode = cx.background_executor().spawn(async move {
      let format = image_format(&mime, &bytes)
        .ok_or_else(|| format!("unsupported image type {mime:?}"))?;
      Image::from_bytes(format, bytes)
        .to_image_data(svg_renderer)
        .map_err(|error| format!("{error:#}"))
    });
    cx.spawn(async move |this, cx| {
      let decoded = decode.await;
      let _ = this.update(cx, |this, cx| {
        this.image_decoded(image_id, generation, decoded, cx)
      });
    })
    .detach();
  }

  fn image_decoded(
    &mut self,
    image_id: u32,
    generation: u64,
    decoded: Result<Arc<RenderImage>, String>,
    cx: &mut Context<Self>,
  ) {
    let Some(entry) = self.images.get_mut(&image_id) else {
      return;
    };
    if entry.generation != generation {
      return;
    }
    entry.state = match decoded {
      Ok(image) => ImageState::Ready(image),
      Err(error) => {
        eprintln!(
          "laufey_gpui: window {}: image {image_id}: {error}",
          self.window_id
        );
        ImageState::Failed
      }
    };
    let users: Vec<u32> = self
      .tree
      .nodes()
      .filter(|node| node.kind == Kind::Image && node.image == image_id)
      .map(|node| node.id)
      .collect();
    for id in users {
      self.report_image(id);
    }
    cx.notify();
  }

  /// `imageLoad` / `imageError` for Image node `id`, once its image has settled.
  fn report_image(&self, id: u32) {
    let Some(node) = self.tree.get(id) else {
      return;
    };
    if !node.listens(listen::IMAGE) || node.image == 0 {
      return;
    }
    let kind = match self.images.get(&node.image).map(|entry| &entry.state) {
      Some(ImageState::Ready(_)) => "imageLoad",
      Some(ImageState::Failed) => "imageError",
      Some(ImageState::Decoding) | None => return,
    };
    self.emit(json!({ "node": id, "type": kind, "image_id": node.image }));
  }

  /// Release a decoded image's texture.
  fn drop_image(
    &mut self,
    entry: ImageEntry,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) {
    if let ImageState::Ready(image) = entry.state {
      cx.drop_image(image, Some(window));
    }
  }

  // --- Snapshot -------------------------------------------------------------------------

  /// The tree as JSON (`native_tree_snapshot` in laufey.h).
  pub fn snapshot(&self, cx: &App) -> String {
    let measured = self.measured.borrow();
    let mut nodes: Vec<&Node> = self.tree.nodes().collect();
    nodes.sort_by_key(|node| node.id);
    let nodes: Vec<serde_json::Value> = nodes
      .into_iter()
      .map(|node| {
        let mut entry = json!({
          "id": node.id,
          "kind": node.kind.name(),
          "parent": node.parent.unwrap_or(0),
          "children": node.children,
          "listen": node.listen,
        });
        if node.kind == Kind::RawText {
          entry["text"] = json!(node.text);
        }
        // A TextInput's text is the field's, which the user edits.
        if let Some(input) = self
          .nodes
          .get(&node.id)
          .and_then(|state| state.input.as_ref())
        {
          entry["text"] = json!(input.read(cx).text());
        }
        if let Some(bounds) = measured.get(&node.id) {
          entry["bounds"] = json!([
            px_f32(bounds.origin.x),
            px_f32(bounds.origin.y),
            px_f32(bounds.size.width),
            px_f32(bounds.size.height),
          ]);
        }
        entry
      })
      .collect();
    json!({
      "seq": self.tree.seq(),
      "root": self.tree.root(),
      "nodes": nodes,
    })
    .to_string()
  }

  // --- After a frame --------------------------------------------------------------------

  /// The frame is painted and `measured` is complete: report layouts that changed and
  /// scroll offsets that moved.
  fn after_paint(&mut self) {
    let measured = self.measured.borrow();
    let mut events = Vec::new();
    for (id, state) in &mut self.nodes {
      let Some(node) = self.tree.get(*id) else {
        continue;
      };
      if let Some(handle) = &state.scroll
        && node.listens(listen::SCROLL)
      {
        // GPUI's offset is the content's (zero or negative); React Native's is the view's.
        let offset = handle.offset();
        let position = (-px_f32(offset.x), -px_f32(offset.y));
        if state.scrolled.is_some_and(|last| last != position) {
          let max = handle.max_offset();
          let viewport = handle.bounds().size;
          events.push(json!({
            "node": id,
            "type": "scroll",
            "x": position.0,
            "y": position.1,
            "contentWidth": px_f32(max.x) + px_f32(viewport.width),
            "contentHeight": px_f32(max.y) + px_f32(viewport.height),
          }));
        }
        state.scrolled = Some(position);
      }
    }
    for node in self.tree.nodes() {
      if !node.listens(listen::LAYOUT) {
        continue;
      }
      let Some(bounds) = measured.get(&node.id) else {
        continue;
      };
      // x and y are in the parent: its origin and, in a ScrollView, its scroll offset are
      // taken out.
      let mut origin = bounds.origin;
      if let Some(parent) = node.parent {
        if let Some(parent_bounds) = measured.get(&parent) {
          origin.x -= parent_bounds.origin.x;
          origin.y -= parent_bounds.origin.y;
        }
        if let Some(handle) = self
          .nodes
          .get(&parent)
          .and_then(|state| state.scroll.as_ref())
        {
          let offset = handle.offset();
          origin.x -= offset.x;
          origin.y -= offset.y;
        }
      }
      let layout = [
        px_f32(origin.x),
        px_f32(origin.y),
        px_f32(bounds.size.width),
        px_f32(bounds.size.height),
      ];
      let state = self.nodes.entry(node.id).or_default();
      if state.layout != Some(layout) {
        state.layout = Some(layout);
        events.push(json!({
          "node": node.id,
          "type": "layout",
          "x": layout[0],
          "y": layout[1],
          "width": layout[2],
          "height": layout[3],
        }));
      }
    }
    drop(measured);
    for event in events {
      self.emit(event);
    }
  }

  // --- Elements -------------------------------------------------------------------------

  /// The element of node `id`, or nothing for a node that draws nothing by itself.
  fn element(&self, id: u32) -> Option<AnyElement> {
    let node = self.tree.get(id)?;
    Some(match node.kind {
      // A RawText outside a Text: plain text in the parent's style.
      Kind::RawText => SharedString::from(node.text.clone()).into_any_element(),
      Kind::Text => {
        let mut el = div();
        style::apply_box(el.style(), node, false);
        style::apply_text(el.style(), node);
        let (text, runs) = self.text_runs(node);
        let el = el.child(
          StyledText::new(SharedString::from(text)).with_highlights(runs),
        );
        self.finish(el, node)
      }
      Kind::View | Kind::Pressable => {
        let mut el = div();
        style::apply_box(el.style(), node, false);
        if node.kind == Kind::Pressable && !disabled(node) {
          el = el.cursor_pointer();
        }
        let el = el.children(self.children(node));
        self.finish(el, node)
      }
      Kind::ScrollView => {
        let mut el = div();
        style::apply_box(el.style(), node, true);
        let horizontal = node
          .prop(Prop::Horizontal)
          .and_then(Value::as_bool)
          .unwrap_or(false);
        let enabled = node
          .prop(Prop::ScrollEnabled)
          .and_then(Value::as_bool)
          .unwrap_or(true);
        let mut el = el
          .id(ElementId::NamedInteger("node".into(), u64::from(id)))
          .children(self.children(node));
        el = match (enabled, horizontal) {
          (false, _) => el.overflow_hidden(),
          (true, false) => el.overflow_y_scroll(),
          (true, true) => el.overflow_x_scroll(),
        };
        if let Some(handle) =
          self.nodes.get(&id).and_then(|state| state.scroll.as_ref())
        {
          el = el.track_scroll(handle);
        }
        self.finish(el, node)
      }
      Kind::Image => {
        let image = match self.images.get(&node.image).map(|entry| &entry.state)
        {
          Some(ImageState::Ready(image)) => Some(image.clone()),
          _ => None,
        };
        match image {
          Some(image) => {
            let mut el = img(ImageSource::Render(image))
              .object_fit(style::object_fit(node));
            style::apply_box(el.style(), node, false);
            self.finish(el, node)
          }
          // No image (yet): the box alone, so the layout does not jump when it arrives.
          None => {
            let mut el = div();
            style::apply_box(el.style(), node, false);
            self.finish(el, node)
          }
        }
      }
      Kind::TextInput => {
        let mut el = div();
        style::apply_box(el.style(), node, false);
        style::apply_text(el.style(), node);
        let input = self
          .nodes
          .get(&id)
          .and_then(|state| state.input.as_ref())
          .cloned();
        let el = el.children(input);
        self.finish(el, node)
      }
    })
  }

  fn children(&self, node: &Node) -> Vec<AnyElement> {
    node
      .children
      .iter()
      .filter_map(|child| self.element(*child))
      .collect()
  }

  /// The text of a Text node and the runs its nested Texts style.
  fn text_runs(
    &self,
    node: &Node,
  ) -> (String, Vec<(Range<usize>, HighlightStyle)>) {
    let mut text = String::new();
    let mut runs = Vec::new();
    self.collect_text(node, &HighlightStyle::default(), &mut text, &mut runs);
    (text, runs)
  }

  fn collect_text(
    &self,
    node: &Node,
    run: &HighlightStyle,
    text: &mut String,
    runs: &mut Vec<(Range<usize>, HighlightStyle)>,
  ) {
    for child in &node.children {
      let Some(child) = self.tree.get(*child) else {
        continue;
      };
      match child.kind {
        Kind::RawText => {
          let start = text.len();
          text.push_str(&child.text);
          if *run != HighlightStyle::default() && text.len() > start {
            runs.push((start..text.len(), *run));
          }
        }
        Kind::Text => {
          let nested = style::merge_run(run, style::run_style(child));
          self.collect_text(child, &nested, text, runs);
        }
        // A view inside text has no place in a run of text.
        _ => {}
      }
    }
  }

  /// The listeners every kind takes (presses, keys), then the measuring wrapper.
  fn finish<E>(&self, mut el: E, node: &Node) -> AnyElement
  where
    E: Element + InteractiveElement,
  {
    let id = node.id;
    let window_id = self.window_id;
    if node.listens(listen::PRESS) && !disabled(node) {
      el = self.pressable(el, id);
    }
    if node.listens(listen::KEY) && node.kind != Kind::TextInput {
      // (A TextInput reports its own keys.)
      if let Some(focus) =
        self.nodes.get(&id).and_then(|state| state.focus.as_ref())
      {
        el = el.track_focus(focus).on_key_down(move |event, _, _| {
          emit_tree_event(
            window_id,
            json!({
              "node": id,
              "type": "key",
              "key": event.keystroke.key,
              "modifiers": modifiers_to_laufey(&event.keystroke.modifiers),
            })
            .to_string(),
          );
        });
      }
    }
    Measured {
      inner: el,
      node: id,
      sink: self.measured.clone(),
    }
    .into_any_element()
  }

  /// Press, as React Native orders it: `pressIn`, then `longPress` after half a second
  /// held, then `pressOut` on release, and `press` when the release is inside and no long
  /// press fired. The innermost listening node takes the press.
  fn pressable<E: InteractiveElement>(&self, el: E, id: u32) -> E {
    let window_id = self.window_id;
    let local = {
      let measured = self.measured.clone();
      move |position: Point<Pixels>| {
        let origin = measured
          .borrow()
          .get(&id)
          .map(|bounds| bounds.origin)
          .unwrap_or_default();
        (px_f32(position.x - origin.x), px_f32(position.y - origin.y))
      }
    };
    let event = move |kind: &str, (x, y): (f32, f32)| {
      emit_tree_event(
        window_id,
        json!({ "node": id, "type": kind, "x": x, "y": y }).to_string(),
      );
    };
    let (down_press, up_press, out_press) =
      (self.press.clone(), self.press.clone(), self.press.clone());
    let (down_local, up_local, out_local) =
      (local.clone(), local.clone(), local);
    el.on_mouse_down(
      MouseButton::Left,
      move |down: &MouseDownEvent, window, cx| {
        cx.stop_propagation();
        let generation = down_press.generations.get() + 1;
        down_press.generations.set(generation);
        *down_press.pressed.borrow_mut() = Some(Pressed {
          node: id,
          generation,
          long: false,
        });
        let at = down_local(down.position);
        event("pressIn", at);
        let press = down_press.clone();
        window
          .spawn(cx, async move |cx| {
            cx.background_executor().timer(LONG_PRESS).await;
            let mut pressed = press.pressed.borrow_mut();
            if let Some(pressed) = pressed.as_mut()
              && pressed.generation == generation
            {
              pressed.long = true;
              event("longPress", at);
            }
          })
          .detach();
      },
    )
    .on_mouse_up(MouseButton::Left, move |up: &MouseUpEvent, _, cx| {
      let released = up_press
        .pressed
        .borrow_mut()
        .take_if(|pressed| pressed.node == id);
      if let Some(pressed) = released {
        cx.stop_propagation();
        let at = up_local(up.position);
        event("pressOut", at);
        if !pressed.long {
          event("press", at);
        }
      }
    })
    .on_mouse_up_out(MouseButton::Left, move |up: &MouseUpEvent, _, _| {
      let released = out_press
        .pressed
        .borrow_mut()
        .take_if(|pressed| pressed.node == id);
      if released.is_some() {
        event("pressOut", out_local(up.position));
      }
    })
  }

  /// The window-level input the runtime registered handlers for.
  fn window_input(&self, mut root: Div) -> Div {
    let interest = host::input_interest();
    let window_id = self.window_id;
    if interest.keyboard {
      root = root
        .on_key_down(move |event: &KeyDownEvent, _, _| {
          let (key, code) = dom_key(&event.keystroke.key);
          host::emit_keyboard(
            window_id,
            true,
            key,
            code,
            modifiers_to_laufey(&event.keystroke.modifiers),
            event.is_held,
          );
        })
        .on_key_up(move |event: &KeyUpEvent, _, _| {
          let (key, code) = dom_key(&event.keystroke.key);
          host::emit_keyboard(
            window_id,
            false,
            key,
            code,
            modifiers_to_laufey(&event.keystroke.modifiers),
            false,
          );
        });
    }
    if interest.mouse_click {
      // Capture phase: a pressable below stops the bubble.
      root = root
        .capture_any_mouse_down(move |event: &MouseDownEvent, _, _| {
          host::emit_mouse_click(
            window_id,
            true,
            button_to_laufey(event.button),
            f64::from(px_f32(event.position.x)),
            f64::from(px_f32(event.position.y)),
            modifiers_to_laufey(&event.modifiers),
            event.click_count as i32,
          );
        })
        .capture_any_mouse_up(move |event: &MouseUpEvent, _, _| {
          host::emit_mouse_click(
            window_id,
            false,
            button_to_laufey(event.button),
            f64::from(px_f32(event.position.x)),
            f64::from(px_f32(event.position.y)),
            modifiers_to_laufey(&event.modifiers),
            event.click_count as i32,
          );
        });
    }
    if interest.mouse_move {
      // One report per frame: the latest position waits for the next frame.
      let pending = self.pending_move.clone();
      root = root.on_mouse_move(move |event: &MouseMoveEvent, window, _| {
        let latest = (
          f64::from(px_f32(event.position.x)),
          f64::from(px_f32(event.position.y)),
          modifiers_to_laufey(&event.modifiers),
        );
        if pending.replace(Some(latest)).is_none() {
          let pending = pending.clone();
          window.on_next_frame(move |_, _| {
            if let Some((x, y, modifiers)) = pending.take() {
              host::emit_mouse_move(window_id, x, y, modifiers);
            }
          });
        }
      });
    }
    if interest.wheel {
      root = root.on_scroll(move |event: &ScrollEvent, _, _| {
        // DOM-signed, as laufey's wheel events are: positive Y scrolls down.
        let (delta, mode) = match event.delta {
          ScrollDelta::Pixels(delta) => {
            ((-px_f32(delta.x), -px_f32(delta.y)), 0)
          }
          ScrollDelta::Lines(delta) => ((-delta.x, -delta.y), 1),
        };
        host::emit_wheel(
          window_id,
          (f64::from(delta.0), f64::from(delta.1)),
          (
            f64::from(px_f32(event.position.x)),
            f64::from(px_f32(event.position.y)),
          ),
          modifiers_to_laufey(&event.modifiers),
          mode,
        );
      });
    }
    root
  }
}

fn disabled(node: &Node) -> bool {
  node
    .prop(Prop::Disabled)
    .and_then(Value::as_bool)
    .unwrap_or(false)
}

/// The image format of IMAGE_DATA bytes: the mime type when GPUI knows it, else the
/// bytes' own signature.
fn image_format(mime: &str, bytes: &[u8]) -> Option<ImageFormat> {
  if let Some(format) = ImageFormat::from_mime_type(mime) {
    return Some(format);
  }
  if bytes.starts_with(b"\x89PNG\r\n\x1a\n") {
    Some(ImageFormat::Png)
  } else if bytes.starts_with(&[0xff, 0xd8, 0xff]) {
    Some(ImageFormat::Jpeg)
  } else if bytes.starts_with(b"GIF87a") || bytes.starts_with(b"GIF89a") {
    Some(ImageFormat::Gif)
  } else if bytes.len() >= 12
    && &bytes[0..4] == b"RIFF"
    && &bytes[8..12] == b"WEBP"
  {
    Some(ImageFormat::Webp)
  } else if bytes.starts_with(b"BM") {
    Some(ImageFormat::Bmp)
  } else {
    None
  }
}

/// A GPUI key name as a DOM `key` and (where it is unambiguous) `code`, the form laufey's
/// keyboard handler takes.
fn dom_key(key: &str) -> (String, String) {
  let named = |key: &str, code: &str| (key.to_string(), code.to_string());
  match key {
    "enter" => named("Enter", "Enter"),
    "escape" => named("Escape", "Escape"),
    "backspace" => named("Backspace", "Backspace"),
    "delete" => named("Delete", "Delete"),
    "tab" => named("Tab", "Tab"),
    "space" => named(" ", "Space"),
    "left" => named("ArrowLeft", "ArrowLeft"),
    "right" => named("ArrowRight", "ArrowRight"),
    "up" => named("ArrowUp", "ArrowUp"),
    "down" => named("ArrowDown", "ArrowDown"),
    "home" => named("Home", "Home"),
    "end" => named("End", "End"),
    "pageup" => named("PageUp", "PageUp"),
    "pagedown" => named("PageDown", "PageDown"),
    _ => {
      let mut chars = key.chars();
      match (chars.next(), chars.next()) {
        (Some(c), None) if c.is_ascii_alphabetic() => {
          (key.to_string(), format!("Key{}", c.to_ascii_uppercase()))
        }
        (Some(c), None) if c.is_ascii_digit() => {
          (key.to_string(), format!("Digit{c}"))
        }
        // F-keys and the rest: the name, capitalized as the DOM has it where they agree.
        (Some(c), Some(_)) if c == 'f' && key[1..].parse::<u8>().is_ok() => {
          let name = key.to_uppercase();
          (name.clone(), name)
        }
        _ => (key.to_string(), String::new()),
      }
    }
  }
}

impl Render for TreeView {
  fn render(
    &mut self,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) -> impl IntoElement {
    if self.reported_size.is_none() {
      self.report_bounds(window);
    }
    // Nodes that listen for keys need a focus handle before they are drawn.
    let wanting_focus: Vec<u32> = self
      .tree
      .nodes()
      .filter(|node| {
        node.listens(listen::KEY)
          && node.kind != Kind::TextInput
          && self.nodes.get(&node.id).is_none_or(|s| s.focus.is_none())
      })
      .map(|node| node.id)
      .collect();
    for id in wanting_focus {
      self.nodes.entry(id).or_default().focus = Some(cx.focus_handle());
    }
    // This frame measures again: a node that is not drawn has no bounds.
    self.measured.borrow_mut().clear();

    let root = div()
      .size_full()
      .flex()
      .flex_col()
      .bg(gpui::white())
      .track_focus(&self.focus);
    let root = self.window_input(root);
    let this = cx.entity();
    root
      .children(self.element(self.tree.root()))
      // Painted last, so every element of the frame has been measured.
      .child(
        canvas(
          |_, _, _| (),
          move |_, _, _, cx| this.update(cx, |this, _| this.after_paint()),
        )
        .absolute()
        .size_0(),
      )
  }
}

/// Records an element's bounds, as laid out, for the node it draws.
struct Measured<E> {
  inner: E,
  node: u32,
  sink: Rc<RefCell<HashMap<u32, Bounds<Pixels>>>>,
}

impl<E: Element> IntoElement for Measured<E> {
  type Element = Self;

  fn into_element(self) -> Self::Element {
    self
  }
}

impl<E: Element> Element for Measured<E> {
  type RequestLayoutState = E::RequestLayoutState;
  type PrepaintState = E::PrepaintState;

  fn id(&self) -> Option<ElementId> {
    self.inner.id()
  }

  fn source_location(&self) -> Option<&'static core::panic::Location<'static>> {
    self.inner.source_location()
  }

  fn request_layout(
    &mut self,
    id: Option<&GlobalElementId>,
    inspector_id: Option<&InspectorElementId>,
    window: &mut Window,
    cx: &mut App,
  ) -> (LayoutId, Self::RequestLayoutState) {
    self.inner.request_layout(id, inspector_id, window, cx)
  }

  fn prepaint(
    &mut self,
    id: Option<&GlobalElementId>,
    inspector_id: Option<&InspectorElementId>,
    bounds: Bounds<Pixels>,
    request_layout: &mut Self::RequestLayoutState,
    window: &mut Window,
    cx: &mut App,
  ) -> Self::PrepaintState {
    self.sink.borrow_mut().insert(self.node, bounds);
    self
      .inner
      .prepaint(id, inspector_id, bounds, request_layout, window, cx)
  }

  fn paint(
    &mut self,
    id: Option<&GlobalElementId>,
    inspector_id: Option<&InspectorElementId>,
    bounds: Bounds<Pixels>,
    request_layout: &mut Self::RequestLayoutState,
    prepaint: &mut Self::PrepaintState,
    window: &mut Window,
    cx: &mut App,
  ) {
    self.inner.paint(
      id,
      inspector_id,
      bounds,
      request_layout,
      prepaint,
      window,
      cx,
    )
  }
}
