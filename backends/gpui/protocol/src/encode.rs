//! The encoder: operations to bytes.

use crate::batch::Op;
use crate::decode::MAGIC;
use crate::ids::{Kind, Prop, StyleProp};
use crate::value::Value;

/// Builds one batch. Nothing is checked here: [`crate::decode`] is the judge.
pub struct BatchBuilder {
  bytes: Vec<u8>,
  op_count: u32,
}

/// Where `op_count` sits in the header.
const OP_COUNT_AT: usize = 12;

impl BatchBuilder {
  pub fn new(seq: u32) -> Self {
    let mut bytes = Vec::with_capacity(256);
    bytes.extend_from_slice(MAGIC);
    bytes.extend_from_slice(&crate::VERSION.to_le_bytes());
    bytes.extend_from_slice(&0u16.to_le_bytes());
    bytes.extend_from_slice(&seq.to_le_bytes());
    bytes.extend_from_slice(&0u32.to_le_bytes());
    Self { bytes, op_count: 0 }
  }

  fn op(&mut self, opcode: u8) -> &mut Self {
    self.op_count += 1;
    self.bytes.push(opcode);
    self
  }

  fn u16(&mut self, v: u16) -> &mut Self {
    self.bytes.extend_from_slice(&v.to_le_bytes());
    self
  }

  fn u32(&mut self, v: u32) -> &mut Self {
    self.bytes.extend_from_slice(&v.to_le_bytes());
    self
  }

  fn blob(&mut self, bytes: &[u8]) -> &mut Self {
    self.u32(bytes.len() as u32);
    self.bytes.extend_from_slice(bytes);
    self
  }

  fn value(&mut self, value: &Value) -> &mut Self {
    match value {
      Value::Unset => self.bytes.push(Value::TAG_UNSET),
      Value::F32(n) => {
        self.bytes.push(Value::TAG_F32);
        self.bytes.extend_from_slice(&n.to_le_bytes());
      }
      Value::Percent(n) => {
        self.bytes.push(Value::TAG_PERCENT);
        self.bytes.extend_from_slice(&n.to_le_bytes());
      }
      Value::Auto => self.bytes.push(Value::TAG_AUTO),
      Value::Enum(v) => {
        self.bytes.push(Value::TAG_ENUM);
        self.bytes.push(*v);
      }
      Value::Color(rgba) => {
        self.bytes.push(Value::TAG_COLOR);
        self.bytes.extend_from_slice(&rgba.to_le_bytes());
      }
      Value::String(s) => {
        self.bytes.push(Value::TAG_STRING);
        self.blob(s.as_bytes());
      }
      Value::Bool(b) => {
        self.bytes.push(Value::TAG_BOOL);
        self.bytes.push(u8::from(*b));
      }
      Value::F64(n) => {
        self.bytes.push(Value::TAG_F64);
        self.bytes.extend_from_slice(&n.to_le_bytes());
      }
    }
    self
  }

  pub fn create(&mut self, id: u32, kind: Kind) -> &mut Self {
    self.op(Op::CREATE).u32(id).u16(kind as u16)
  }

  /// `before` 0 appends.
  pub fn insert(&mut self, parent: u32, child: u32, before: u32) -> &mut Self {
    self.op(Op::INSERT).u32(parent).u32(child).u32(before)
  }

  pub fn remove(&mut self, parent: u32, child: u32) -> &mut Self {
    self.op(Op::REMOVE).u32(parent).u32(child)
  }

  pub fn destroy(&mut self, id: u32) -> &mut Self {
    self.op(Op::DESTROY).u32(id)
  }

  pub fn set_root(&mut self, id: u32) -> &mut Self {
    self.op(Op::SET_ROOT).u32(id)
  }

  pub fn set_style(
    &mut self,
    id: u32,
    props: &[(StyleProp, Value)],
  ) -> &mut Self {
    self.op(Op::SET_STYLE).u32(id).u16(props.len() as u16);
    for (prop, value) in props {
      self.u16(*prop as u16).value(value);
    }
    self
  }

  pub fn set_prop(&mut self, id: u32, prop: Prop, value: &Value) -> &mut Self {
    self.op(Op::SET_PROP).u32(id).u16(prop as u16).value(value)
  }

  pub fn set_text(&mut self, id: u32, text: &str) -> &mut Self {
    self.op(Op::SET_TEXT).u32(id).blob(text.as_bytes())
  }

  pub fn listen(&mut self, id: u32, mask: u32) -> &mut Self {
    self.op(Op::LISTEN).u32(id).u32(mask)
  }

  pub fn set_image(&mut self, id: u32, image_id: u32) -> &mut Self {
    self.op(Op::SET_IMAGE).u32(id).u32(image_id)
  }

  pub fn image_data(
    &mut self,
    image_id: u32,
    mime: &str,
    bytes: &[u8],
  ) -> &mut Self {
    self
      .op(Op::IMAGE_DATA)
      .u32(image_id)
      .blob(mime.as_bytes())
      .blob(bytes)
  }

  pub fn image_free(&mut self, image_id: u32) -> &mut Self {
    self.op(Op::IMAGE_FREE).u32(image_id)
  }

  /// One operation as [`crate::decode`] returns it.
  pub fn push(&mut self, op: &Op) -> &mut Self {
    match op {
      Op::Create { id, kind } => self.create(*id, *kind),
      Op::Insert {
        parent,
        child,
        before,
      } => self.insert(*parent, *child, *before),
      Op::Remove { parent, child } => self.remove(*parent, *child),
      Op::Destroy { id } => self.destroy(*id),
      Op::SetRoot { id } => self.set_root(*id),
      Op::SetStyle { id, props } => self.set_style(*id, props),
      Op::SetProp { id, prop, value } => self.set_prop(*id, *prop, value),
      Op::SetText { id, text } => self.set_text(*id, text),
      Op::Listen { id, mask } => self.listen(*id, *mask),
      Op::SetImage { id, image_id } => self.set_image(*id, *image_id),
      Op::ImageData {
        image_id,
        mime,
        bytes,
      } => self.image_data(*image_id, mime, bytes),
      Op::ImageFree { image_id } => self.image_free(*image_id),
    }
  }

  /// The number of operations so far.
  pub fn len(&self) -> u32 {
    self.op_count
  }

  pub fn is_empty(&self) -> bool {
    self.op_count == 0
  }

  /// The batch's bytes.
  pub fn finish(&self) -> Vec<u8> {
    let mut bytes = self.bytes.clone();
    bytes[OP_COUNT_AT..OP_COUNT_AT + 4]
      .copy_from_slice(&self.op_count.to_le_bytes());
    bytes
  }
}
