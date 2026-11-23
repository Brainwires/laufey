//! Prop values and which value each prop takes.

use crate::ids::{Prop, StyleProp};

/// A prop value. On the wire: a tag byte, then the payload.
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
  /// Tag 0: reset the prop to its default.
  Unset,
  /// Tag 1: a number; for a length, logical pixels.
  F32(f32),
  /// Tag 2: a percentage (`50.0` is 50%).
  Percent(f32),
  /// Tag 3: `auto`.
  Auto,
  /// Tag 4: an enum value from the prop's table.
  Enum(u8),
  /// Tag 5: `0xRRGGBBAA`, straight alpha.
  Color(u32),
  /// Tag 6.
  String(String),
  /// Tag 7.
  Bool(bool),
  /// Tag 8: a number that needs more than an f32 (an event count).
  F64(f64),
}

impl Value {
  pub const TAG_UNSET: u8 = 0;
  pub const TAG_F32: u8 = 1;
  pub const TAG_PERCENT: u8 = 2;
  pub const TAG_AUTO: u8 = 3;
  pub const TAG_ENUM: u8 = 4;
  pub const TAG_COLOR: u8 = 5;
  pub const TAG_STRING: u8 = 6;
  pub const TAG_BOOL: u8 = 7;
  pub const TAG_F64: u8 = 8;

  /// The number of an `F32` or `F64` value.
  pub fn as_number(&self) -> Option<f64> {
    match self {
      Value::F32(n) => Some(f64::from(*n)),
      Value::F64(n) => Some(*n),
      _ => None,
    }
  }

  /// [`Value::as_number`] as an f32.
  pub fn as_f32(&self) -> Option<f32> {
    self.as_number().map(|n| n as f32)
  }

  pub fn as_enum(&self) -> Option<u8> {
    match self {
      Value::Enum(v) => Some(*v),
      _ => None,
    }
  }

  pub fn as_color(&self) -> Option<u32> {
    match self {
      Value::Color(rgba) => Some(*rgba),
      _ => None,
    }
  }

  pub fn as_str(&self) -> Option<&str> {
    match self {
      Value::String(s) => Some(s),
      _ => None,
    }
  }

  pub fn as_bool(&self) -> Option<bool> {
    match self {
      Value::Bool(b) => Some(*b),
      _ => None,
    }
  }
}

/// What a prop takes besides `unset`, which every prop takes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ValueClass {
  /// One of these enum values.
  Enum(&'static [u8]),
  /// A finite number (`f32` or `f64`).
  Number,
  /// A finite number, a percentage, or `auto`.
  Length,
  /// A finite number or a percentage.
  LengthPercent,
  Color,
  String,
  Bool,
}

impl ValueClass {
  /// Whether `value` is one this class takes (`unset` always is).
  pub fn accepts(self, value: &Value) -> bool {
    match (self, value) {
      (_, Value::Unset) => true,
      (ValueClass::Enum(values), Value::Enum(v)) => values.contains(v),
      (ValueClass::Number, Value::F32(n)) => n.is_finite(),
      (ValueClass::Number, Value::F64(n)) => n.is_finite(),
      (
        ValueClass::Length | ValueClass::LengthPercent,
        Value::F32(n) | Value::Percent(n),
      ) => n.is_finite(),
      (ValueClass::Length, Value::Auto) => true,
      (ValueClass::Color, Value::Color(_)) => true,
      (ValueClass::String, Value::String(_)) => true,
      (ValueClass::Bool, Value::Bool(_)) => true,
      _ => false,
    }
  }
}

impl StyleProp {
  /// What this style prop takes. Exhaustive on purpose: a prop appended to `props.json` does
  /// not compile until it is given a class here.
  pub const fn class(self) -> ValueClass {
    use StyleProp::*;
    match self {
      Display | FlexDirection | FlexWrap | JustifyContent | AlignItems
      | AlignSelf | AlignContent | Position | Overflow | FontStyle
      | TextAlign | TextDecorationLine => match self.enum_values() {
        Some(values) => ValueClass::Enum(values),
        None => panic!("an enum-valued style prop without a table"),
      },
      Flex
      | FlexGrow
      | FlexShrink
      | AspectRatio
      | Opacity
      | FontSize
      | FontWeight
      | LineHeight
      | LetterSpacing
      | BorderTopWidth
      | BorderRightWidth
      | BorderBottomWidth
      | BorderLeftWidth
      | BorderTopLeftRadius
      | BorderTopRightRadius
      | BorderBottomRightRadius
      | BorderBottomLeftRadius => ValueClass::Number,
      FlexBasis | Top | Right | Bottom | Left | Width | Height | MinWidth
      | MinHeight | MaxWidth | MaxHeight | MarginTop | MarginRight
      | MarginBottom | MarginLeft => ValueClass::Length,
      Gap | RowGap | ColumnGap | PaddingTop | PaddingRight | PaddingBottom
      | PaddingLeft => ValueClass::LengthPercent,
      BackgroundColor | BorderTopColor | BorderRightColor
      | BorderBottomColor | BorderLeftColor | Color => ValueClass::Color,
      FontFamily => ValueClass::String,
    }
  }
}

impl Prop {
  /// What this non-style prop takes. Exhaustive on purpose, as [`StyleProp::class`].
  pub const fn class(self) -> ValueClass {
    match self {
      Prop::Value
      | Prop::Placeholder
      | Prop::AccessibilityLabel
      | Prop::AccessibilityRole
      | Prop::TestId => ValueClass::String,
      Prop::Editable
      | Prop::SecureTextEntry
      | Prop::Multiline
      | Prop::Horizontal
      | Prop::ScrollEnabled
      | Prop::Disabled => ValueClass::Bool,
      Prop::EventCount => ValueClass::Number,
      Prop::ResizeMode => match self.enum_values() {
        Some(values) => ValueClass::Enum(values),
        None => panic!("an enum-valued prop without a table"),
      },
      Prop::PlaceholderTextColor => ValueClass::Color,
    }
  }
}
