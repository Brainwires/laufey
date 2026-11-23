//! Bytes to a [`Batch`].
//!
//! ```text
//! header:  magic "NTB1" | version u16 = 1 | flags u16 = 0 | seq u32 | op_count u32
//! op:      opcode u8 | payload
//! ```
//!
//! Integers are little-endian; a string is `u32 byte_len` then UTF-8. Decoding checks
//! everything that needs no tree: the header, every opcode, kind, prop id, value tag and value
//! class, the ids that may not be 0, the LISTEN bits, UTF-8, and that the bytes hold exactly
//! `op_count` operations. One fault rejects the batch.

use crate::batch::{Batch, Op};
use crate::ids::{Kind, Prop, StyleProp, listen};
use crate::value::Value;

/// The first four bytes of a batch.
pub const MAGIC: &[u8; 4] = b"NTB1";

/// Why a batch does not decode. `at` is the byte offset of the fault.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DecodeError {
  /// The bytes end inside the header or an operation.
  Truncated {
    at: usize,
  },
  BadMagic,
  UnsupportedVersion(u16),
  /// `flags` must be 0 in version 1.
  UnsupportedFlags(u16),
  UnknownOpcode {
    at: usize,
    opcode: u8,
  },
  UnknownKind {
    at: usize,
    kind: u16,
  },
  UnknownStyleProp {
    at: usize,
    prop: u16,
  },
  UnknownProp {
    at: usize,
    prop: u16,
  },
  UnknownValueTag {
    at: usize,
    tag: u8,
  },
  /// The value is not one the prop takes (the wrong tag, an enum value outside the prop's
  /// table, a number that is not finite, a bool that is not 0 or 1).
  BadValue {
    at: usize,
    prop: &'static str,
  },
  /// An id that may not be 0.
  ZeroId {
    at: usize,
  },
  /// A LISTEN mask with bits outside the table.
  BadMask {
    at: usize,
    mask: u32,
  },
  BadUtf8 {
    at: usize,
  },
  /// Bytes after the last operation.
  TrailingBytes {
    at: usize,
  },
}

impl std::fmt::Display for DecodeError {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match self {
      DecodeError::Truncated { at } => write!(f, "truncated at byte {at}"),
      DecodeError::BadMagic => f.write_str("not an NTB1 batch"),
      DecodeError::UnsupportedVersion(v) => write!(f, "version {v}"),
      DecodeError::UnsupportedFlags(flags) => write!(f, "flags {flags:#x}"),
      DecodeError::UnknownOpcode { at, opcode } => {
        write!(f, "unknown opcode {opcode} at byte {at}")
      }
      DecodeError::UnknownKind { at, kind } => {
        write!(f, "unknown kind {kind} at byte {at}")
      }
      DecodeError::UnknownStyleProp { at, prop } => {
        write!(f, "unknown style prop {prop} at byte {at}")
      }
      DecodeError::UnknownProp { at, prop } => {
        write!(f, "unknown prop {prop} at byte {at}")
      }
      DecodeError::UnknownValueTag { at, tag } => {
        write!(f, "unknown value tag {tag} at byte {at}")
      }
      DecodeError::BadValue { at, prop } => {
        write!(f, "a value {prop} does not take at byte {at}")
      }
      DecodeError::ZeroId { at } => write!(f, "id 0 at byte {at}"),
      DecodeError::BadMask { at, mask } => {
        write!(f, "listen mask {mask:#x} at byte {at}")
      }
      DecodeError::BadUtf8 { at } => write!(f, "invalid UTF-8 at byte {at}"),
      DecodeError::TrailingBytes { at } => {
        write!(f, "bytes after the last operation at byte {at}")
      }
    }
  }
}

impl std::error::Error for DecodeError {}

struct Reader<'a> {
  bytes: &'a [u8],
  at: usize,
}

impl<'a> Reader<'a> {
  fn take(&mut self, len: usize) -> Result<&'a [u8], DecodeError> {
    let end = self
      .at
      .checked_add(len)
      .filter(|end| *end <= self.bytes.len())
      .ok_or(DecodeError::Truncated { at: self.at })?;
    let slice = &self.bytes[self.at..end];
    self.at = end;
    Ok(slice)
  }

  fn u8(&mut self) -> Result<u8, DecodeError> {
    Ok(self.take(1)?[0])
  }

  fn u16(&mut self) -> Result<u16, DecodeError> {
    let b = self.take(2)?;
    Ok(u16::from_le_bytes([b[0], b[1]]))
  }

  fn u32(&mut self) -> Result<u32, DecodeError> {
    let b = self.take(4)?;
    Ok(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
  }

  fn f32(&mut self) -> Result<f32, DecodeError> {
    Ok(f32::from_bits(self.u32()?))
  }

  fn f64(&mut self) -> Result<f64, DecodeError> {
    let b = self.take(8)?;
    let mut raw = [0u8; 8];
    raw.copy_from_slice(b);
    Ok(f64::from_le_bytes(raw))
  }

  /// An id that may not be 0.
  fn id(&mut self) -> Result<u32, DecodeError> {
    let at = self.at;
    match self.u32()? {
      0 => Err(DecodeError::ZeroId { at }),
      id => Ok(id),
    }
  }

  fn blob(&mut self) -> Result<&'a [u8], DecodeError> {
    let len = self.u32()? as usize;
    self.take(len)
  }

  fn string(&mut self) -> Result<String, DecodeError> {
    let at = self.at;
    let bytes = self.blob()?;
    match std::str::from_utf8(bytes) {
      Ok(s) => Ok(s.to_owned()),
      Err(_) => Err(DecodeError::BadUtf8 { at }),
    }
  }

  fn value(&mut self) -> Result<Value, DecodeError> {
    let at = self.at;
    Ok(match self.u8()? {
      Value::TAG_UNSET => Value::Unset,
      Value::TAG_F32 => Value::F32(self.f32()?),
      Value::TAG_PERCENT => Value::Percent(self.f32()?),
      Value::TAG_AUTO => Value::Auto,
      Value::TAG_ENUM => Value::Enum(self.u8()?),
      Value::TAG_COLOR => Value::Color(self.u32()?),
      Value::TAG_STRING => Value::String(self.string()?),
      Value::TAG_BOOL => match self.u8()? {
        0 => Value::Bool(false),
        1 => Value::Bool(true),
        _ => return Err(DecodeError::BadValue { at, prop: "bool" }),
      },
      Value::TAG_F64 => Value::F64(self.f64()?),
      tag => return Err(DecodeError::UnknownValueTag { at, tag }),
    })
  }
}

/// Decode a whole batch, or nothing.
pub fn decode(bytes: &[u8]) -> Result<Batch, DecodeError> {
  let mut r = Reader { bytes, at: 0 };
  let magic = r.take(4).map_err(|_| DecodeError::BadMagic)?;
  if magic != MAGIC {
    return Err(DecodeError::BadMagic);
  }
  let version = r.u16()?;
  if version != crate::VERSION {
    return Err(DecodeError::UnsupportedVersion(version));
  }
  let flags = r.u16()?;
  if flags != 0 {
    return Err(DecodeError::UnsupportedFlags(flags));
  }
  let seq = r.u32()?;
  let op_count = r.u32()? as usize;
  // The smallest operation is 5 bytes: a count the bytes cannot hold reserves nothing.
  let mut ops = Vec::with_capacity(op_count.min((bytes.len() - r.at) / 5));
  for _ in 0..op_count {
    ops.push(op(&mut r)?);
  }
  if r.at != bytes.len() {
    return Err(DecodeError::TrailingBytes { at: r.at });
  }
  Ok(Batch { seq, ops })
}

fn op(r: &mut Reader<'_>) -> Result<Op, DecodeError> {
  let at = r.at;
  Ok(match r.u8()? {
    Op::CREATE => {
      let id = r.id()?;
      let at = r.at;
      let kind = r.u16()?;
      let kind =
        Kind::from_u16(kind).ok_or(DecodeError::UnknownKind { at, kind })?;
      Op::Create { id, kind }
    }
    Op::INSERT => Op::Insert {
      parent: r.id()?,
      child: r.id()?,
      before: r.u32()?,
    },
    Op::REMOVE => Op::Remove {
      parent: r.id()?,
      child: r.id()?,
    },
    Op::DESTROY => Op::Destroy { id: r.id()? },
    Op::SET_ROOT => Op::SetRoot { id: r.u32()? },
    Op::SET_STYLE => {
      let id = r.id()?;
      let count = r.u16()? as usize;
      let mut props = Vec::with_capacity(count.min(StyleProp::ALL.len()));
      for _ in 0..count {
        let at = r.at;
        let prop = r.u16()?;
        let prop = StyleProp::from_u16(prop)
          .ok_or(DecodeError::UnknownStyleProp { at, prop })?;
        let at = r.at;
        let value = r.value()?;
        if !prop.class().accepts(&value) {
          return Err(DecodeError::BadValue {
            at,
            prop: prop.name(),
          });
        }
        props.push((prop, value));
      }
      Op::SetStyle { id, props }
    }
    Op::SET_PROP => {
      let id = r.id()?;
      let at = r.at;
      let prop = r.u16()?;
      let prop =
        Prop::from_u16(prop).ok_or(DecodeError::UnknownProp { at, prop })?;
      let at = r.at;
      let value = r.value()?;
      if !prop.class().accepts(&value) {
        return Err(DecodeError::BadValue {
          at,
          prop: prop.name(),
        });
      }
      Op::SetProp { id, prop, value }
    }
    Op::SET_TEXT => Op::SetText {
      id: r.id()?,
      text: r.string()?,
    },
    Op::LISTEN => {
      let id = r.id()?;
      let at = r.at;
      let mask = r.u32()?;
      if mask & !listen::ALL != 0 {
        return Err(DecodeError::BadMask { at, mask });
      }
      Op::Listen { id, mask }
    }
    Op::SET_IMAGE => Op::SetImage {
      id: r.id()?,
      image_id: r.u32()?,
    },
    Op::IMAGE_DATA => Op::ImageData {
      image_id: r.id()?,
      mime: r.string()?,
      bytes: r.blob()?.to_vec(),
    },
    Op::IMAGE_FREE => Op::ImageFree { image_id: r.id()? },
    opcode => return Err(DecodeError::UnknownOpcode { at, opcode }),
  })
}
