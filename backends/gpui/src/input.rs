//! A TextInput node: a single-line editable text field on GPUI's input handling, IME
//! included (marked text, the candidate window's position, the character palette).
//!
//! The field's state lives here, in its own entity, not in the tree: the runtime sets
//! `value` and the user types, and React Native's `eventCount` settles the race. Each edit
//! by the user bumps the count and reports it with `changeText`; a `value` from the runtime
//! is taken only when the `eventCount` it carries has caught up with the field's.
//!
//! Not done in v1: `multiline` (the field stays one line), selection by double click or
//! drag past the edge, undo.

use std::ops::Range;

use gpui::{
  App, Bounds, ClipboardItem, Context, CursorStyle, Element, ElementId,
  ElementInputHandler, Entity, EntityInputHandler, FocusHandle, Focusable,
  GlobalElementId, Hsla, InspectorElementId, IntoElement, KeyBinding,
  KeyDownEvent, LayoutId, MouseButton, MouseDownEvent, MouseMoveEvent,
  MouseUpEvent, PaintQuad, Pixels, Point, Render, ShapedLine, SharedString,
  Style, TextRun, UTF16Selection, UnderlineStyle, Window, actions, div, fill,
  point, prelude::*, px, relative, size,
};
use laufey_ntb1::listen;
use serde_json::json;
use unicode_segmentation::UnicodeSegmentation as _;

use crate::api::modifiers_to_laufey;
use crate::host::emit_tree_event;

actions!(
  laufey_text_input,
  [
    Backspace,
    Delete,
    Left,
    Right,
    SelectLeft,
    SelectRight,
    SelectAll,
    Home,
    End,
    ShowCharacterPalette,
    Paste,
    Cut,
    Copy,
    Submit,
  ]
);

/// The key context the bindings below are scoped to.
const KEY_CONTEXT: &str = "LaufeyTextInput";

/// What a secure field shows for each character.
const BULLET: &str = "\u{2022}";

/// Bind the editing keys: Command on macOS and Control elsewhere for select all, copy, cut
/// and paste. (A static binding cannot say `secondary-`.)
pub fn bind_keys(cx: &mut App) {
  let context = Some(KEY_CONTEXT);
  #[cfg(target_os = "macos")]
  let (all, paste, copy, cut) = ("cmd-a", "cmd-v", "cmd-c", "cmd-x");
  #[cfg(not(target_os = "macos"))]
  let (all, paste, copy, cut) = ("ctrl-a", "ctrl-v", "ctrl-c", "ctrl-x");
  cx.bind_keys([
    KeyBinding::new("backspace", Backspace, context),
    KeyBinding::new("delete", Delete, context),
    KeyBinding::new("left", Left, context),
    KeyBinding::new("right", Right, context),
    KeyBinding::new("shift-left", SelectLeft, context),
    KeyBinding::new("shift-right", SelectRight, context),
    KeyBinding::new(all, SelectAll, context),
    KeyBinding::new(paste, Paste, context),
    KeyBinding::new(copy, Copy, context),
    KeyBinding::new(cut, Cut, context),
    KeyBinding::new("home", Home, context),
    KeyBinding::new("end", End, context),
    KeyBinding::new("enter", Submit, context),
    #[cfg(target_os = "macos")]
    KeyBinding::new("ctrl-cmd-space", ShowCharacterPalette, context),
  ]);
}

/// The node's props, as the view reads them from the tree after each batch.
#[derive(Clone, PartialEq)]
pub struct InputProps {
  pub placeholder: SharedString,
  pub placeholder_color: Option<Hsla>,
  pub editable: bool,
  pub secure: bool,
  /// The node's LISTEN mask.
  pub listen: u32,
}

impl Default for InputProps {
  fn default() -> Self {
    Self {
      placeholder: SharedString::default(),
      placeholder_color: None,
      editable: true,
      secure: false,
      listen: 0,
    }
  }
}

pub struct TextInput {
  window_id: u32,
  node: u32,
  focus_handle: FocusHandle,
  content: String,
  props: InputProps,
  /// How many edits the user made: React Native's native event count.
  event_count: u64,
  selected_range: Range<usize>,
  selection_reversed: bool,
  marked_range: Option<Range<usize>>,
  /// The selection last reported, in UTF-16 units.
  reported_selection: Option<(usize, usize)>,
  last_layout: Option<ShapedLine>,
  last_bounds: Option<Bounds<Pixels>>,
  is_selecting: bool,
}

impl TextInput {
  pub fn new(
    window_id: u32,
    node: u32,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) -> Self {
    let focus_handle = cx.focus_handle();
    cx.on_focus(&focus_handle, window, |this, _, cx| {
      this.emit(listen::FOCUS, json!({ "node": this.node, "type": "focus" }));
      cx.notify();
    })
    .detach();
    cx.on_blur(&focus_handle, window, |this, _, cx| {
      this.is_selecting = false;
      this.emit(listen::FOCUS, json!({ "node": this.node, "type": "blur" }));
      cx.notify();
    })
    .detach();
    Self {
      window_id,
      node,
      focus_handle,
      content: String::new(),
      props: InputProps::default(),
      event_count: 0,
      selected_range: 0..0,
      selection_reversed: false,
      marked_range: None,
      reported_selection: None,
      last_layout: None,
      last_bounds: None,
      is_selecting: false,
    }
  }

  /// The field's text (for the tree snapshot).
  pub fn text(&self) -> &str {
    &self.content
  }

  fn emit(&self, bit: u32, event: serde_json::Value) {
    if self.props.listen & bit != 0 {
      emit_tree_event(self.window_id, event.to_string());
    }
  }

  /// Take the node's props. `value` is the runtime's text when this batch set it, with the
  /// `eventCount` the node carries: it is dropped when the user has edited since.
  pub fn sync(
    &mut self,
    props: InputProps,
    value: Option<(&str, Option<u64>)>,
    cx: &mut Context<Self>,
  ) {
    let mut changed = false;
    if props != self.props {
      self.props = props;
      changed = true;
    }
    if let Some((value, event_count)) = value
      && event_count.is_none_or(|count| count >= self.event_count)
      && value != self.content
    {
      self.content = value.to_owned();
      let end = self.content.len();
      self.selected_range =
        self.selected_range.start.min(end)..self.selected_range.end.min(end);
      self.snap_selection();
      self.marked_range = None;
      changed = true;
    }
    if changed {
      cx.notify();
    }
  }

  /// Keep the selection on character boundaries after the text was replaced.
  fn snap_selection(&mut self) {
    let snap = |mut offset: usize, text: &str| {
      while !text.is_char_boundary(offset) {
        offset -= 1;
      }
      offset
    };
    self.selected_range = snap(self.selected_range.start, &self.content)
      ..snap(self.selected_range.end, &self.content);
  }

  /// The user changed the text.
  fn edited(&mut self, window: &Window, cx: &mut Context<Self>) {
    self.event_count += 1;
    self.emit(
      listen::TEXT,
      json!({
        "node": self.node,
        "type": "changeText",
        "text": self.content,
        "eventCount": self.event_count,
      }),
    );
    window.notify_text_input_changed(&self.focus_handle);
    self.selection_changed(window, cx);
  }

  fn selection_changed(&mut self, window: &Window, cx: &mut Context<Self>) {
    window.notify_text_selection_changed(&self.focus_handle);
    let range = self.range_to_utf16(&self.selected_range);
    let selection = (range.start, range.end);
    if self.reported_selection != Some(selection) {
      self.reported_selection = Some(selection);
      self.emit(
        listen::TEXT,
        json!({
          "node": self.node,
          "type": "selectionChange",
          "start": selection.0,
          "end": selection.1,
        }),
      );
    }
    cx.notify();
  }

  // --- Actions ---

  fn left(&mut self, _: &Left, window: &mut Window, cx: &mut Context<Self>) {
    if self.selected_range.is_empty() {
      self.move_to(self.previous_boundary(self.cursor_offset()), window, cx);
    } else {
      self.move_to(self.selected_range.start, window, cx)
    }
  }

  fn right(&mut self, _: &Right, window: &mut Window, cx: &mut Context<Self>) {
    if self.selected_range.is_empty() {
      self.move_to(self.next_boundary(self.selected_range.end), window, cx);
    } else {
      self.move_to(self.selected_range.end, window, cx)
    }
  }

  fn select_left(
    &mut self,
    _: &SelectLeft,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) {
    self.select_to(self.previous_boundary(self.cursor_offset()), window, cx);
  }

  fn select_right(
    &mut self,
    _: &SelectRight,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) {
    self.select_to(self.next_boundary(self.cursor_offset()), window, cx);
  }

  fn select_all(
    &mut self,
    _: &SelectAll,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) {
    self.move_to(0, window, cx);
    self.select_to(self.content.len(), window, cx)
  }

  fn home(&mut self, _: &Home, window: &mut Window, cx: &mut Context<Self>) {
    self.move_to(0, window, cx);
  }

  fn end(&mut self, _: &End, window: &mut Window, cx: &mut Context<Self>) {
    self.move_to(self.content.len(), window, cx);
  }

  fn backspace(
    &mut self,
    _: &Backspace,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) {
    if self.selected_range.is_empty() {
      let previous = self.previous_boundary(self.cursor_offset());
      if self.cursor_offset() == previous {
        return;
      }
      self.select_to(previous, window, cx)
    }
    self.replace_text_in_range(None, "", window, cx)
  }

  fn delete(
    &mut self,
    _: &Delete,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) {
    if self.selected_range.is_empty() {
      let next = self.next_boundary(self.cursor_offset());
      if self.cursor_offset() == next {
        return;
      }
      self.select_to(next, window, cx)
    }
    self.replace_text_in_range(None, "", window, cx)
  }

  fn submit(&mut self, _: &Submit, _: &mut Window, _: &mut Context<Self>) {
    self.emit(
      listen::TEXT,
      json!({ "node": self.node, "type": "submitEditing" }),
    );
  }

  fn show_character_palette(
    &mut self,
    _: &ShowCharacterPalette,
    window: &mut Window,
    _: &mut Context<Self>,
  ) {
    window.show_character_palette();
  }

  fn paste(&mut self, _: &Paste, window: &mut Window, cx: &mut Context<Self>) {
    if let Some(text) = cx.read_from_clipboard().and_then(|item| item.text()) {
      // One line: line breaks become spaces.
      let text = text.replace(['\n', '\r'], " ");
      self.replace_text_in_range(None, &text, window, cx);
    }
  }

  fn copy(&mut self, _: &Copy, _: &mut Window, cx: &mut Context<Self>) {
    // A secure field gives nothing to the clipboard.
    if !self.selected_range.is_empty() && !self.props.secure {
      cx.write_to_clipboard(ClipboardItem::new_string(
        self.content[self.selected_range.clone()].to_string(),
      ));
    }
  }

  fn cut(&mut self, _: &Cut, window: &mut Window, cx: &mut Context<Self>) {
    if !self.selected_range.is_empty() && !self.props.secure {
      cx.write_to_clipboard(ClipboardItem::new_string(
        self.content[self.selected_range.clone()].to_string(),
      ));
      self.replace_text_in_range(None, "", window, cx)
    }
  }

  fn on_key_down(
    &mut self,
    event: &KeyDownEvent,
    _: &mut Window,
    _: &mut Context<Self>,
  ) {
    self.emit(
      listen::KEY,
      json!({
        "node": self.node,
        "type": "key",
        "key": event.keystroke.key,
        "modifiers": modifiers_to_laufey(&event.keystroke.modifiers),
      }),
    );
  }

  fn on_mouse_down(
    &mut self,
    event: &MouseDownEvent,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) {
    self.is_selecting = true;
    window.request_virtual_keyboard();
    let index = self.index_for_mouse_position(event.position);
    if event.modifiers.shift {
      self.select_to(index, window, cx);
    } else {
      self.move_to(index, window, cx)
    }
  }

  fn on_mouse_up(
    &mut self,
    _: &MouseUpEvent,
    _: &mut Window,
    _: &mut Context<Self>,
  ) {
    self.is_selecting = false;
  }

  fn on_mouse_move(
    &mut self,
    event: &MouseMoveEvent,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) {
    if self.is_selecting {
      self.select_to(self.index_for_mouse_position(event.position), window, cx);
    }
  }

  // --- Selection ---

  fn move_to(
    &mut self,
    offset: usize,
    window: &Window,
    cx: &mut Context<Self>,
  ) {
    self.selected_range = offset..offset;
    self.selection_reversed = false;
    self.selection_changed(window, cx);
  }

  fn select_to(
    &mut self,
    offset: usize,
    window: &Window,
    cx: &mut Context<Self>,
  ) {
    if self.selection_reversed {
      self.selected_range.start = offset
    } else {
      self.selected_range.end = offset
    };
    if self.selected_range.end < self.selected_range.start {
      self.selection_reversed = !self.selection_reversed;
      self.selected_range = self.selected_range.end..self.selected_range.start;
    }
    self.selection_changed(window, cx);
  }

  fn cursor_offset(&self) -> usize {
    if self.selection_reversed {
      self.selected_range.start
    } else {
      self.selected_range.end
    }
  }

  fn previous_boundary(&self, offset: usize) -> usize {
    self
      .content
      .grapheme_indices(true)
      .rev()
      .find_map(|(index, _)| (index < offset).then_some(index))
      .unwrap_or(0)
  }

  fn next_boundary(&self, offset: usize) -> usize {
    self
      .content
      .grapheme_indices(true)
      .find_map(|(index, _)| (index > offset).then_some(index))
      .unwrap_or(self.content.len())
  }

  // --- Content offsets, UTF-16 offsets, and offsets in the text on screen ---

  fn offset_from_utf16(&self, offset: usize) -> usize {
    let mut utf8_offset = 0;
    let mut utf16_count = 0;
    for ch in self.content.chars() {
      if utf16_count >= offset {
        break;
      }
      utf16_count += ch.len_utf16();
      utf8_offset += ch.len_utf8();
    }
    utf8_offset
  }

  fn offset_to_utf16(&self, offset: usize) -> usize {
    let mut utf16_offset = 0;
    let mut utf8_count = 0;
    for ch in self.content.chars() {
      if utf8_count >= offset {
        break;
      }
      utf8_count += ch.len_utf8();
      utf16_offset += ch.len_utf16();
    }
    utf16_offset
  }

  fn range_to_utf16(&self, range: &Range<usize>) -> Range<usize> {
    self.offset_to_utf16(range.start)..self.offset_to_utf16(range.end)
  }

  fn range_from_utf16(&self, range_utf16: &Range<usize>) -> Range<usize> {
    self.offset_from_utf16(range_utf16.start)
      ..self.offset_from_utf16(range_utf16.end)
  }

  /// The text drawn: the content, or one bullet per character of a secure field.
  fn display_text(&self) -> SharedString {
    if self.props.secure {
      BULLET.repeat(self.content.chars().count()).into()
    } else {
      self.content.clone().into()
    }
  }

  /// A content offset as an offset in [`Self::display_text`].
  fn display_offset(&self, offset: usize) -> usize {
    if self.props.secure {
      self.content[..offset.min(self.content.len())]
        .chars()
        .count()
        * BULLET.len()
    } else {
      offset
    }
  }

  /// The inverse of [`Self::display_offset`].
  fn content_offset(&self, offset: usize) -> usize {
    if self.props.secure {
      self
        .content
        .char_indices()
        .nth(offset / BULLET.len())
        .map(|(index, _)| index)
        .unwrap_or(self.content.len())
    } else {
      offset.min(self.content.len())
    }
  }

  fn index_for_mouse_position(&self, position: Point<Pixels>) -> usize {
    if self.content.is_empty() {
      return 0;
    }
    let (Some(bounds), Some(line)) =
      (self.last_bounds.as_ref(), self.last_layout.as_ref())
    else {
      return 0;
    };
    if position.y < bounds.top() {
      return 0;
    }
    if position.y > bounds.bottom() {
      return self.content.len();
    }
    self.content_offset(line.closest_index_for_x(position.x - bounds.left()))
  }

  /// Whether the laid-out line is the one for the text as it is now.
  fn layout_is_current(&self) -> bool {
    self
      .last_layout
      .as_ref()
      .is_some_and(|line| line.text == self.display_text())
  }
}

impl EntityInputHandler for TextInput {
  fn text_for_range(
    &mut self,
    range_utf16: Range<usize>,
    actual_range: &mut Option<Range<usize>>,
    _window: &mut Window,
    _cx: &mut Context<Self>,
  ) -> Option<String> {
    let range = self.range_from_utf16(&range_utf16);
    actual_range.replace(self.range_to_utf16(&range));
    // The OS must not read a secure field back.
    if self.props.secure {
      return Some(BULLET.repeat(self.content[range].chars().count()));
    }
    Some(self.content[range].to_string())
  }

  fn selected_text_range(
    &mut self,
    _ignore_disabled_input: bool,
    _window: &mut Window,
    _cx: &mut Context<Self>,
  ) -> Option<UTF16Selection> {
    Some(UTF16Selection {
      range: self.range_to_utf16(&self.selected_range),
      reversed: self.selection_reversed,
    })
  }

  fn set_selected_text_range(
    &mut self,
    range_utf16: Range<usize>,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) {
    self.selected_range = self.range_from_utf16(&range_utf16);
    self.selection_reversed = false;
    self.selection_changed(window, cx);
  }

  fn text_length_utf16(
    &mut self,
    _window: &mut Window,
    _cx: &mut Context<Self>,
  ) -> Option<usize> {
    Some(self.content.encode_utf16().count())
  }

  fn marked_text_range(
    &self,
    _window: &mut Window,
    _cx: &mut Context<Self>,
  ) -> Option<Range<usize>> {
    self
      .marked_range
      .as_ref()
      .map(|range| self.range_to_utf16(range))
  }

  fn unmark_text(&mut self, window: &mut Window, cx: &mut Context<Self>) {
    self.marked_range = None;
    window.notify_text_input_changed(&self.focus_handle);
    cx.notify();
  }

  fn replace_text_in_range(
    &mut self,
    range_utf16: Option<Range<usize>>,
    new_text: &str,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) {
    if !self.props.editable {
      return;
    }
    let range = range_utf16
      .as_ref()
      .map(|range_utf16| self.range_from_utf16(range_utf16))
      .or(self.marked_range.clone())
      .unwrap_or(self.selected_range.clone());
    self.content.replace_range(range.clone(), new_text);
    let cursor = range.start + new_text.len();
    self.selected_range = cursor..cursor;
    self.selection_reversed = false;
    self.marked_range.take();
    self.edited(window, cx);
  }

  fn replace_and_mark_text_in_range(
    &mut self,
    range_utf16: Option<Range<usize>>,
    new_text: &str,
    new_selected_range_utf16: Option<Range<usize>>,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) {
    if !self.props.editable {
      return;
    }
    let range = range_utf16
      .as_ref()
      .map(|range_utf16| self.range_from_utf16(range_utf16))
      .or(self.marked_range.clone())
      .unwrap_or(self.selected_range.clone());
    self.content.replace_range(range.clone(), new_text);
    self.marked_range =
      (!new_text.is_empty()).then(|| range.start..range.start + new_text.len());
    let insertion_start_utf16 = self.offset_to_utf16(range.start);
    let inserted_length_utf16 = new_text.encode_utf16().count();
    self.selected_range = new_selected_range_utf16
      .map(|selected| {
        self.range_from_utf16(
          &(insertion_start_utf16 + selected.start.min(inserted_length_utf16)
            ..insertion_start_utf16 + selected.end.min(inserted_length_utf16)),
        )
      })
      .unwrap_or_else(|| {
        range.start + new_text.len()..range.start + new_text.len()
      });
    self.selection_reversed = false;
    self.edited(window, cx);
  }

  fn bounds_for_range(
    &mut self,
    range_utf16: Range<usize>,
    bounds: Bounds<Pixels>,
    _window: &mut Window,
    _cx: &mut Context<Self>,
  ) -> Option<Bounds<Pixels>> {
    if !self.layout_is_current() {
      return None;
    }
    let range = self.range_from_utf16(&range_utf16);
    let (start, end) = (
      self.display_offset(range.start),
      self.display_offset(range.end),
    );
    let line = self.last_layout.as_ref()?;
    Some(Bounds::from_corners(
      point(bounds.left() + line.x_for_index(start), bounds.top()),
      point(bounds.left() + line.x_for_index(end), bounds.bottom()),
    ))
  }

  fn selection_bounds_for_range(
    &mut self,
    range_utf16: Range<usize>,
    bounds: Bounds<Pixels>,
    window: &mut Window,
    cx: &mut Context<Self>,
  ) -> Vec<Bounds<Pixels>> {
    self
      .bounds_for_range(range_utf16, bounds, window, cx)
      .into_iter()
      .collect()
  }

  fn character_index_for_point(
    &mut self,
    point: Point<Pixels>,
    _window: &mut Window,
    _cx: &mut Context<Self>,
  ) -> Option<usize> {
    let line_point = self.last_bounds?.localize(&point)?;
    if !self.layout_is_current() {
      return None;
    }
    let index = self.last_layout.as_ref()?.closest_index_for_x(line_point.x);
    Some(self.offset_to_utf16(self.content_offset(index)))
  }
}

/// The line of text, the selection and the caret.
struct TextElement {
  input: Entity<TextInput>,
}

struct PrepaintState {
  line: Option<ShapedLine>,
  cursor: Option<PaintQuad>,
  selection: Option<PaintQuad>,
}

impl IntoElement for TextElement {
  type Element = Self;

  fn into_element(self) -> Self::Element {
    self
  }
}

impl Element for TextElement {
  type RequestLayoutState = ();
  type PrepaintState = PrepaintState;

  fn id(&self) -> Option<ElementId> {
    None
  }

  fn source_location(&self) -> Option<&'static core::panic::Location<'static>> {
    None
  }

  fn request_layout(
    &mut self,
    _id: Option<&GlobalElementId>,
    _inspector_id: Option<&InspectorElementId>,
    window: &mut Window,
    cx: &mut App,
  ) -> (LayoutId, Self::RequestLayoutState) {
    let mut style = Style::default();
    style.size.width = relative(1.).into();
    style.size.height = window.line_height().into();
    (window.request_layout(style, [], cx), ())
  }

  fn prepaint(
    &mut self,
    _id: Option<&GlobalElementId>,
    _inspector_id: Option<&InspectorElementId>,
    bounds: Bounds<Pixels>,
    _request_layout: &mut Self::RequestLayoutState,
    window: &mut Window,
    cx: &mut App,
  ) -> Self::PrepaintState {
    let input = self.input.read(cx);
    let style = window.text_style();
    let empty = input.content.is_empty();
    let (display_text, text_color) = if empty {
      (
        input.props.placeholder.clone(),
        input
          .props
          .placeholder_color
          .unwrap_or_else(|| style.color.opacity(0.4)),
      )
    } else {
      (input.display_text(), style.color)
    };
    let selected = if empty {
      0..0
    } else {
      input.display_offset(input.selected_range.start)
        ..input.display_offset(input.selected_range.end)
    };
    let cursor = if empty {
      0
    } else {
      input.display_offset(input.cursor_offset())
    };
    let marked = input.marked_range.as_ref().filter(|_| !empty).map(|range| {
      input.display_offset(range.start)..input.display_offset(range.end)
    });

    let run = TextRun {
      len: display_text.len(),
      font: style.font(),
      color: text_color,
      background_color: None,
      underline: None,
      strikethrough: None,
    };
    let runs = if let Some(marked) = marked {
      vec![
        TextRun {
          len: marked.start,
          ..run.clone()
        },
        TextRun {
          len: marked.end - marked.start,
          underline: Some(UnderlineStyle {
            color: Some(run.color),
            thickness: px(1.0),
            wavy: false,
          }),
          ..run.clone()
        },
        TextRun {
          len: display_text.len() - marked.end,
          ..run
        },
      ]
      .into_iter()
      .filter(|run| run.len > 0)
      .collect()
    } else {
      vec![run]
    };

    let font_size = style.font_size.to_pixels(window.rem_size());
    let line =
      window
        .text_system()
        .shape_line(display_text, font_size, &runs, None);

    let (selection, cursor) = if selected.is_empty() {
      (
        None,
        Some(fill(
          Bounds::new(
            point(bounds.left() + line.x_for_index(cursor), bounds.top()),
            size(px(1.5), bounds.bottom() - bounds.top()),
          ),
          style.color,
        )),
      )
    } else {
      (
        Some(fill(
          Bounds::from_corners(
            point(
              bounds.left() + line.x_for_index(selected.start),
              bounds.top(),
            ),
            point(
              bounds.left() + line.x_for_index(selected.end),
              bounds.bottom(),
            ),
          ),
          gpui::rgba(0x3b82f655),
        )),
        None,
      )
    };
    PrepaintState {
      line: Some(line),
      cursor,
      selection,
    }
  }

  fn paint(
    &mut self,
    _id: Option<&GlobalElementId>,
    _inspector_id: Option<&InspectorElementId>,
    bounds: Bounds<Pixels>,
    _request_layout: &mut Self::RequestLayoutState,
    prepaint: &mut Self::PrepaintState,
    window: &mut Window,
    cx: &mut App,
  ) {
    let focus_handle = self.input.read(cx).focus_handle.clone();
    window.handle_input(
      &focus_handle,
      ElementInputHandler::new(bounds, self.input.clone()),
      cx,
    );
    if let Some(selection) = prepaint.selection.take() {
      window.paint_quad(selection)
    }
    let Some(line) = prepaint.line.take() else {
      return;
    };
    // A line that cannot be painted leaves the field empty for this frame.
    let _ = line.paint(
      bounds.origin,
      window.line_height(),
      gpui::TextAlign::Left,
      None,
      window,
      cx,
    );
    if focus_handle.is_focused(window)
      && let Some(cursor) = prepaint.cursor.take()
    {
      window.paint_quad(cursor);
    }
    self.input.update(cx, |input, _cx| {
      input.last_layout = Some(line);
      input.last_bounds = Some(bounds);
    });
  }
}

impl Render for TextInput {
  fn render(
    &mut self,
    _window: &mut Window,
    cx: &mut Context<Self>,
  ) -> impl IntoElement {
    div()
      .flex()
      .w_full()
      .overflow_hidden()
      .key_context(KEY_CONTEXT)
      .track_focus(&self.focus_handle(cx))
      .cursor(CursorStyle::IBeam)
      .on_action(cx.listener(Self::backspace))
      .on_action(cx.listener(Self::delete))
      .on_action(cx.listener(Self::left))
      .on_action(cx.listener(Self::right))
      .on_action(cx.listener(Self::select_left))
      .on_action(cx.listener(Self::select_right))
      .on_action(cx.listener(Self::select_all))
      .on_action(cx.listener(Self::home))
      .on_action(cx.listener(Self::end))
      .on_action(cx.listener(Self::submit))
      .on_action(cx.listener(Self::show_character_palette))
      .on_action(cx.listener(Self::paste))
      .on_action(cx.listener(Self::cut))
      .on_action(cx.listener(Self::copy))
      .on_key_down(cx.listener(Self::on_key_down))
      .on_mouse_down(MouseButton::Left, cx.listener(Self::on_mouse_down))
      .on_mouse_up(MouseButton::Left, cx.listener(Self::on_mouse_up))
      .on_mouse_up_out(MouseButton::Left, cx.listener(Self::on_mouse_up))
      .on_mouse_move(cx.listener(Self::on_mouse_move))
      .child(TextElement { input: cx.entity() })
  }
}

impl Focusable for TextInput {
  fn focus_handle(&self, _: &App) -> FocusHandle {
    self.focus_handle.clone()
  }
}
