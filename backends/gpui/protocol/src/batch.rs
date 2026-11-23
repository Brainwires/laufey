//! A decoded batch.

use crate::ids::{Kind, Prop, StyleProp};
use crate::value::Value;

/// One operation. Node id 0 means "none" where an id may be absent.
#[derive(Debug, Clone, PartialEq)]
pub enum Op {
  /// Opcode 1.
  Create { id: u32, kind: Kind },
  /// Opcode 2. `before` 0 appends. Inserting an attached child moves it.
  Insert {
    parent: u32,
    child: u32,
    before: u32,
  },
  /// Opcode 3. Detaches; the node stays alive.
  Remove { parent: u32, child: u32 },
  /// Opcode 4. Frees the node and its subtree.
  Destroy { id: u32 },
  /// Opcode 5. `id` 0 clears the root.
  SetRoot { id: u32 },
  /// Opcode 6. Sets only the listed props.
  SetStyle {
    id: u32,
    props: Vec<(StyleProp, Value)>,
  },
  /// Opcode 7.
  SetProp { id: u32, prop: Prop, value: Value },
  /// Opcode 8. RawText content.
  SetText { id: u32, text: String },
  /// Opcode 9. Replaces the node's event mask.
  Listen { id: u32, mask: u32 },
  /// Opcode 10. `image_id` 0 clears the node's image.
  SetImage { id: u32, image_id: u32 },
  /// Opcode 11.
  ImageData {
    image_id: u32,
    mime: String,
    bytes: Vec<u8>,
  },
  /// Opcode 12.
  ImageFree { image_id: u32 },
}

impl Op {
  pub const CREATE: u8 = 1;
  pub const INSERT: u8 = 2;
  pub const REMOVE: u8 = 3;
  pub const DESTROY: u8 = 4;
  pub const SET_ROOT: u8 = 5;
  pub const SET_STYLE: u8 = 6;
  pub const SET_PROP: u8 = 7;
  pub const SET_TEXT: u8 = 8;
  pub const LISTEN: u8 = 9;
  pub const SET_IMAGE: u8 = 10;
  pub const IMAGE_DATA: u8 = 11;
  pub const IMAGE_FREE: u8 = 12;
}

/// A whole batch: applied atomically.
#[derive(Debug, Clone, PartialEq)]
pub struct Batch {
  /// The sender's sequence number, echoed in a `batchError` event.
  pub seq: u32,
  pub ops: Vec<Op>,
}
