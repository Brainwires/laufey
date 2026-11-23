//! The B0 wire format: one JSON tree per commit, replacing the previous one. (B1 replaces this
//! with mutation batches in a shared buffer.)

use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum NodeType {
  View,
  Text,
  Pressable,
}

#[derive(Debug, Clone, Deserialize)]
pub struct Node {
  pub id: u32,
  #[serde(rename = "type")]
  pub kind: NodeType,
  #[serde(default)]
  pub style: Style,
  #[serde(default)]
  pub text: Option<String>,
  #[serde(default)]
  pub children: Vec<Node>,
}

/// The React Native style subset B0 understands. Lengths are logical pixels; colours are
/// `#rrggbb`.
#[derive(Debug, Clone, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Style {
  pub flex: Option<f32>,
  pub flex_direction: Option<FlexDirection>,
  pub align_items: Option<Align>,
  pub justify_content: Option<Align>,
  pub padding: Option<f32>,
  pub gap: Option<f32>,
  pub width: Option<f32>,
  pub height: Option<f32>,
  pub background_color: Option<String>,
  pub color: Option<String>,
  pub font_size: Option<f32>,
  pub border_radius: Option<f32>,
}

#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum FlexDirection {
  Row,
  Column,
}

#[derive(Debug, Clone, Copy, Deserialize)]
#[serde(rename_all = "kebab-case")]
pub enum Align {
  FlexStart,
  Center,
  FlexEnd,
}

/// An input event, host to JS.
#[derive(Debug, Clone, Serialize)]
#[serde(tag = "type", rename_all = "lowercase")]
pub enum UiEvent {
  Press { id: u32 },
}

impl Node {
  /// The first node of `kind`, depth first.
  pub fn find(&self, kind: NodeType) -> Option<&Node> {
    if self.kind == kind {
      return Some(self);
    }
    self.children.iter().find_map(|child| child.find(kind))
  }

  /// The node's own text and its descendants' text, in order.
  pub fn text_content(&self) -> String {
    let mut out = self.text.clone().unwrap_or_default();
    for child in &self.children {
      out.push_str(&child.text_content());
    }
    out
  }
}

/// `#rrggbb` as `0xRRGGBB`.
pub fn parse_color(value: &str) -> Option<u32> {
  let hex = value.strip_prefix('#')?;
  if hex.len() != 6 {
    return None;
  }
  u32::from_str_radix(hex, 16).ok()
}
