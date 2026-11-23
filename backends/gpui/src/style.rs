//! NTB1 style props on GPUI styles.
//!
//! The props are React Native's (`props.json`), so are the defaults: every node is a flex
//! container, a column, and does not shrink. Text props apply to Text and TextInput nodes
//! only and do not cascade from a View, as in React Native.
//!
//! Not expressible in GPUI 0.3 (see docs/native-tree.md): per-side border colours (the
//! first set side colours the whole border), `letterSpacing`, `textAlign: justify`,
//! `resizeMode: repeat`.

use gpui::{
  AbsoluteLength, AlignContent, AlignItems, DefiniteLength, Display, Fill,
  FlexDirection, FlexWrap, FontStyle, FontWeight, HighlightStyle, Hsla,
  JustifyContent, Length, ObjectFit, Overflow, Position, SharedString,
  StrikethroughStyle, StyleRefinement, TextAlign, UnderlineStyle, px, rgba,
};
use laufey_ntb1::ids::enums;
use laufey_ntb1::{Node, Prop, StyleProp, Value};

/// React Native's default text size and colour.
pub const DEFAULT_FONT_SIZE: f32 = 14.0;
pub const DEFAULT_TEXT_COLOR: u32 = 0x000000ff;

/// `0xRRGGBBAA`, straight alpha.
pub fn color(rgba_value: u32) -> Hsla {
  rgba(rgba_value).into()
}

fn absolute(v: f32) -> AbsoluteLength {
  AbsoluteLength::Pixels(px(v))
}

fn definite(value: &Value) -> Option<DefiniteLength> {
  match value {
    Value::F32(v) => Some(DefiniteLength::Absolute(absolute(*v))),
    Value::F64(v) => Some(DefiniteLength::Absolute(absolute(*v as f32))),
    Value::Percent(v) => Some(DefiniteLength::Fraction(*v / 100.0)),
    _ => None,
  }
}

fn length(value: &Value) -> Option<Length> {
  match value {
    Value::Auto => Some(Length::Auto),
    other => definite(other).map(Length::Definite),
  }
}

fn align(value: u8) -> Option<AlignItems> {
  use enums::align_items as v;
  Some(match value {
    v::FLEX_START => AlignItems::FlexStart,
    v::FLEX_END => AlignItems::FlexEnd,
    v::CENTER => AlignItems::Center,
    v::STRETCH => AlignItems::Stretch,
    v::BASELINE => AlignItems::Baseline,
    // `auto`, and the space-* values that only alignContent has.
    _ => return None,
  })
}

fn align_content(value: u8) -> Option<AlignContent> {
  use enums::align_content as v;
  Some(match value {
    v::FLEX_START => AlignContent::FlexStart,
    v::FLEX_END => AlignContent::FlexEnd,
    v::CENTER => AlignContent::Center,
    v::STRETCH => AlignContent::Stretch,
    v::SPACE_BETWEEN => AlignContent::SpaceBetween,
    v::SPACE_AROUND => AlignContent::SpaceAround,
    v::SPACE_EVENLY => AlignContent::SpaceEvenly,
    _ => return None,
  })
}

fn justify(value: u8) -> JustifyContent {
  use enums::justify_content as v;
  match value {
    v::FLEX_END => JustifyContent::FlexEnd,
    v::CENTER => JustifyContent::Center,
    v::SPACE_BETWEEN => JustifyContent::SpaceBetween,
    v::SPACE_AROUND => JustifyContent::SpaceAround,
    v::SPACE_EVENLY => JustifyContent::SpaceEvenly,
    _ => JustifyContent::FlexStart,
  }
}

/// The layout and paint props of `node` on `style`, over React Native's defaults.
/// `scrolls` is a ScrollView, whose overflow the caller sets.
pub fn apply_box(style: &mut StyleRefinement, node: &Node, scrolls: bool) {
  style.display = Some(Display::Flex);
  style.flex_direction = Some(FlexDirection::Column);
  style.flex_shrink = Some(0.0);
  style.position = Some(Position::Relative);

  // `flex` first: the longhands override it whatever the order they were set in.
  if let Some(flex) = node.style(StyleProp::Flex).and_then(Value::as_f32) {
    if flex > 0.0 {
      style.flex_grow = Some(flex);
      style.flex_shrink = Some(1.0);
      style.flex_basis =
        Some(Length::Definite(DefiniteLength::Absolute(absolute(0.0))));
    } else if flex < 0.0 {
      style.flex_shrink = Some(-flex);
    }
  }

  let mut border_color = None;
  for (prop, value) in &node.style {
    let number = value.as_f32();
    let enumerated = value.as_enum();
    match prop {
      StyleProp::Display => {
        style.display = Some(match enumerated {
          Some(enums::display::NONE) => Display::None,
          _ => Display::Flex,
        });
      }
      StyleProp::FlexDirection => {
        use enums::flex_direction as v;
        style.flex_direction = Some(match enumerated {
          Some(v::ROW) => FlexDirection::Row,
          Some(v::ROW_REVERSE) => FlexDirection::RowReverse,
          Some(v::COLUMN_REVERSE) => FlexDirection::ColumnReverse,
          _ => FlexDirection::Column,
        });
      }
      StyleProp::FlexWrap => {
        use enums::flex_wrap as v;
        style.flex_wrap = Some(match enumerated {
          Some(v::WRAP) => FlexWrap::Wrap,
          Some(v::WRAP_REVERSE) => FlexWrap::WrapReverse,
          _ => FlexWrap::NoWrap,
        });
      }
      StyleProp::Flex => {}
      StyleProp::FlexGrow => style.flex_grow = number,
      StyleProp::FlexShrink => style.flex_shrink = number,
      StyleProp::FlexBasis => style.flex_basis = length(value),
      StyleProp::JustifyContent => {
        style.justify_content = enumerated.map(justify);
      }
      StyleProp::AlignItems => style.align_items = enumerated.and_then(align),
      StyleProp::AlignSelf => style.align_self = enumerated.and_then(align),
      StyleProp::AlignContent => {
        style.align_content = enumerated.and_then(align_content);
      }
      StyleProp::Gap => {
        style.gap.width = definite(value);
        style.gap.height = definite(value);
      }
      // rowGap is the gap between rows (the vertical one), columnGap between columns.
      StyleProp::RowGap => style.gap.height = definite(value),
      StyleProp::ColumnGap => style.gap.width = definite(value),
      StyleProp::Position => {
        style.position = Some(match enumerated {
          Some(enums::position::ABSOLUTE) => Position::Absolute,
          _ => Position::Relative,
        });
      }
      StyleProp::Top => style.inset.top = length(value),
      StyleProp::Right => style.inset.right = length(value),
      StyleProp::Bottom => style.inset.bottom = length(value),
      StyleProp::Left => style.inset.left = length(value),
      StyleProp::Width => style.size.width = length(value),
      StyleProp::Height => style.size.height = length(value),
      StyleProp::MinWidth => style.min_size.width = length(value),
      StyleProp::MinHeight => style.min_size.height = length(value),
      StyleProp::MaxWidth => style.max_size.width = length(value),
      StyleProp::MaxHeight => style.max_size.height = length(value),
      StyleProp::AspectRatio => style.aspect_ratio = number,
      StyleProp::MarginTop => style.margin.top = length(value),
      StyleProp::MarginRight => style.margin.right = length(value),
      StyleProp::MarginBottom => style.margin.bottom = length(value),
      StyleProp::MarginLeft => style.margin.left = length(value),
      StyleProp::PaddingTop => style.padding.top = definite(value),
      StyleProp::PaddingRight => style.padding.right = definite(value),
      StyleProp::PaddingBottom => style.padding.bottom = definite(value),
      StyleProp::PaddingLeft => style.padding.left = definite(value),
      StyleProp::BorderTopWidth => {
        style.border_widths.top = number.map(absolute);
      }
      StyleProp::BorderRightWidth => {
        style.border_widths.right = number.map(absolute);
      }
      StyleProp::BorderBottomWidth => {
        style.border_widths.bottom = number.map(absolute);
      }
      StyleProp::BorderLeftWidth => {
        style.border_widths.left = number.map(absolute);
      }
      StyleProp::Overflow => {
        if !scrolls {
          let overflow = match enumerated {
            Some(enums::overflow::VISIBLE) | None => Overflow::Visible,
            // `scroll` outside a ScrollView clips: only a ScrollView scrolls.
            _ => Overflow::Hidden,
          };
          style.overflow.x = Some(overflow);
          style.overflow.y = Some(overflow);
        }
      }
      StyleProp::BackgroundColor => {
        style.background = value.as_color().map(|c| Fill::from(color(c)));
      }
      StyleProp::BorderTopColor
      | StyleProp::BorderRightColor
      | StyleProp::BorderBottomColor
      | StyleProp::BorderLeftColor => {
        // GPUI has one border colour: the first side set (top, right, bottom, left).
        if border_color.is_none() {
          border_color = value.as_color().map(color);
        }
      }
      StyleProp::BorderTopLeftRadius => {
        style.corner_radii.top_left = number.map(absolute);
      }
      StyleProp::BorderTopRightRadius => {
        style.corner_radii.top_right = number.map(absolute);
      }
      StyleProp::BorderBottomRightRadius => {
        style.corner_radii.bottom_right = number.map(absolute);
      }
      StyleProp::BorderBottomLeftRadius => {
        style.corner_radii.bottom_left = number.map(absolute);
      }
      StyleProp::Opacity => style.opacity = number.map(|v| v.clamp(0.0, 1.0)),
      // Text props: apply_text, for the kinds that draw text.
      StyleProp::Color
      | StyleProp::FontFamily
      | StyleProp::FontSize
      | StyleProp::FontWeight
      | StyleProp::FontStyle
      | StyleProp::LineHeight
      | StyleProp::LetterSpacing
      | StyleProp::TextAlign
      | StyleProp::TextDecorationLine => {}
    }
  }
  if let Some(border_color) = border_color {
    style.border_color = Some(border_color);
  }
}

fn decoration(
  value: Option<u8>,
) -> (Option<UnderlineStyle>, Option<StrikethroughStyle>) {
  use enums::text_decoration_line as v;
  let underline = || UnderlineStyle {
    thickness: px(1.0),
    color: None,
    wavy: false,
  };
  let strikethrough = || StrikethroughStyle {
    thickness: px(1.0),
    color: None,
  };
  match value {
    Some(v::UNDERLINE) => (Some(underline()), None),
    Some(v::LINE_THROUGH) => (None, Some(strikethrough())),
    Some(v::UNDERLINE_LINE_THROUGH) => {
      (Some(underline()), Some(strikethrough()))
    }
    _ => (None, None),
  }
}

fn font_weight(node: &Node) -> Option<FontWeight> {
  node
    .style(StyleProp::FontWeight)
    .and_then(Value::as_f32)
    .map(|w| FontWeight(w.clamp(1.0, 1000.0)))
}

fn font_style(node: &Node) -> Option<FontStyle> {
  node
    .style(StyleProp::FontStyle)
    .and_then(Value::as_enum)
    .map(|v| {
      if v == enums::font_style::ITALIC {
        FontStyle::Italic
      } else {
        FontStyle::Normal
      }
    })
}

/// The text props of a Text or TextInput node on `style`, over React Native's defaults
/// (set explicitly, so nothing cascades in from an ancestor).
pub fn apply_text(style: &mut StyleRefinement, node: &Node) {
  let text = &mut style.text;
  text.color = Some(color(
    node
      .style(StyleProp::Color)
      .and_then(Value::as_color)
      .unwrap_or(DEFAULT_TEXT_COLOR),
  ));
  let font_size = node
    .style(StyleProp::FontSize)
    .and_then(Value::as_f32)
    .unwrap_or(DEFAULT_FONT_SIZE);
  text.font_size = Some(absolute(font_size));
  text.line_height = Some(
    match node.style(StyleProp::LineHeight).and_then(Value::as_f32) {
      Some(line_height) => DefiniteLength::Absolute(absolute(line_height)),
      None => DefiniteLength::Fraction(1.2),
    },
  );
  if let Some(family) =
    node.style(StyleProp::FontFamily).and_then(Value::as_str)
  {
    text.font_family = Some(SharedString::from(family.to_owned()));
  }
  text.font_weight = Some(font_weight(node).unwrap_or(FontWeight::NORMAL));
  text.font_style = Some(font_style(node).unwrap_or(FontStyle::Normal));
  text.text_align = Some(
    match node.style(StyleProp::TextAlign).and_then(Value::as_enum) {
      Some(enums::text_align::RIGHT) => TextAlign::Right,
      Some(enums::text_align::CENTER) => TextAlign::Center,
      _ => TextAlign::Left,
    },
  );
  let (underline, strikethrough) = decoration(
    node
      .style(StyleProp::TextDecorationLine)
      .and_then(Value::as_enum),
  );
  text.underline = underline;
  text.strikethrough = strikethrough;
}

/// What a Text nested in a Text changes for its run: the props a run can carry.
pub fn run_style(node: &Node) -> HighlightStyle {
  let (underline, strikethrough) = decoration(
    node
      .style(StyleProp::TextDecorationLine)
      .and_then(Value::as_enum),
  );
  HighlightStyle {
    color: node
      .style(StyleProp::Color)
      .and_then(Value::as_color)
      .map(color),
    font_weight: font_weight(node),
    font_style: font_style(node),
    background_color: node
      .style(StyleProp::BackgroundColor)
      .and_then(Value::as_color)
      .map(color),
    underline,
    strikethrough,
    ..Default::default()
  }
}

/// `inner` over `outer`: a nested run keeps what it does not set.
pub fn merge_run(
  outer: &HighlightStyle,
  inner: HighlightStyle,
) -> HighlightStyle {
  HighlightStyle {
    color: inner.color.or(outer.color),
    font_weight: inner.font_weight.or(outer.font_weight),
    font_style: inner.font_style.or(outer.font_style),
    background_color: inner.background_color.or(outer.background_color),
    underline: inner.underline.or(outer.underline),
    strikethrough: inner.strikethrough.or(outer.strikethrough),
    fade_out: inner.fade_out.or(outer.fade_out),
  }
}

/// An Image's `resizeMode` (React Native's default is `cover`).
pub fn object_fit(node: &Node) -> ObjectFit {
  use enums::resize_mode as v;
  match node.prop(Prop::ResizeMode).and_then(Value::as_enum) {
    Some(v::CONTAIN) => ObjectFit::Contain,
    Some(v::STRETCH) => ObjectFit::Fill,
    Some(v::CENTER) => ObjectFit::ScaleDown,
    // `repeat` has no GPUI counterpart.
    _ => ObjectFit::Cover,
  }
}
