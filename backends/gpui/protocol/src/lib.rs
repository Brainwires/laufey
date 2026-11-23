//! NTB1: the batch format of laufey's native tree (`docs/native-tree.md`), its id tables, and
//! the retained tree a batch mutates.
//!
//! - [`ids`]: the kinds, prop ids, enum values and LISTEN bits, generated from `props.json`.
//! - [`decode`]: bytes to a [`Batch`], validating everything that can be checked without the
//!   tree. Nothing is returned for a batch with any fault.
//! - [`Tree`]: the retained tree. [`Tree::apply`] checks the whole batch against the tree
//!   before it changes anything, so a rejected batch leaves the tree as it was.
//! - [`BatchBuilder`]: the encoder (the reference for the TypeScript one in denext).
//!
//! No dependencies and no UI types: a host maps [`Node`]s onto its own elements.

mod batch;
mod decode;
mod encode;
mod tree;
mod value;

/// The id tables, generated from `props.json`: a byte-for-byte copy of thalora-web-browser
/// `docs/b1-props.json` @ 7debb58.
pub mod ids {
  include!(concat!(env!("OUT_DIR"), "/ids.rs"));
}

pub use batch::{Batch, Op};
pub use decode::{DecodeError, MAGIC, decode};
pub use encode::BatchBuilder;
pub use ids::{Kind, Prop, StyleProp, listen};
pub use tree::{Applied, ApplyError, ImageOp, Node, Tree};
pub use value::{Value, ValueClass};

/// The batch format version this crate reads and writes.
pub const VERSION: u16 = ids::VERSION;

#[cfg(test)]
mod tests;
