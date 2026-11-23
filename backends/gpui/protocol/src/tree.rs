//! The retained tree a batch mutates.
//!
//! [`Tree::apply`] is atomic: the whole batch is checked against the tree first (on an overlay
//! that records what the batch would create, move and destroy, without touching the tree), and
//! only a batch with no fault is applied.
//!
//! What a batch must respect, beyond what [`crate::decode`] checks:
//!
//! - CREATE: the id is not in use (ids are never reused, so an id destroyed earlier in the same
//!   batch is still in use).
//! - INSERT: parent and child are alive and different; the parent's kind takes children (View,
//!   Text, ScrollView, Pressable); the child is not the root and not an ancestor of the parent;
//!   `before` is 0 or another child of the parent.
//! - REMOVE: the child is alive and attached to that parent.
//! - DESTROY: the node is alive. Its whole subtree goes with it; an attached node is detached
//!   first, and the root is cleared when it was in the subtree.
//! - SET_ROOT: 0, or an alive node with no parent.
//! - SET_STYLE, SET_PROP, LISTEN: the node is alive.
//! - SET_TEXT: the node is an alive RawText.
//! - SET_IMAGE: the node is an alive Image (the image may arrive later).
//! - IMAGE_DATA, IMAGE_FREE: nothing; a host keeps images by id.

use std::collections::{BTreeMap, HashMap, HashSet};

use crate::batch::{Batch, Op};
use crate::ids::{Kind, Prop, StyleProp};
use crate::value::Value;

/// One node of the retained tree.
#[derive(Debug, Clone, PartialEq)]
pub struct Node {
  pub id: u32,
  pub kind: Kind,
  /// The parent's id while attached.
  pub parent: Option<u32>,
  /// The children, in order.
  pub children: Vec<u32>,
  /// The style props that are set (an `unset` removes the entry).
  pub style: BTreeMap<StyleProp, Value>,
  /// The non-style props that are set.
  pub props: BTreeMap<Prop, Value>,
  /// A RawText's content.
  pub text: String,
  /// The LISTEN mask.
  pub listen: u32,
  /// An Image's image id, 0 for none.
  pub image: u32,
}

impl Node {
  fn new(id: u32, kind: Kind) -> Self {
    Self {
      id,
      kind,
      parent: None,
      children: Vec::new(),
      style: BTreeMap::new(),
      props: BTreeMap::new(),
      text: String::new(),
      listen: 0,
      image: 0,
    }
  }

  pub fn style(&self, prop: StyleProp) -> Option<&Value> {
    self.style.get(&prop)
  }

  pub fn prop(&self, prop: Prop) -> Option<&Value> {
    self.props.get(&prop)
  }

  /// Whether the node subscribed to any of `bits`.
  pub fn listens(&self, bits: u32) -> bool {
    self.listen & bits != 0
  }
}

/// Whether nodes of `kind` take children.
pub const fn takes_children(kind: Kind) -> bool {
  match kind {
    Kind::View | Kind::Text | Kind::ScrollView | Kind::Pressable => true,
    Kind::RawText | Kind::Image | Kind::TextInput => false,
  }
}

/// Why a batch does not fit the tree. `op` is the index of the operation at fault.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ApplyError {
  /// CREATE of an id that is in use.
  IdInUse { op: usize, id: u32 },
  /// An id that names no alive node.
  UnknownNode { op: usize, id: u32 },
  /// The node's kind does not take this operation.
  WrongKind { op: usize, id: u32, kind: Kind },
  /// INSERT of a node under itself or under one of its descendants.
  Cycle { op: usize, parent: u32, child: u32 },
  /// INSERT of the root, or SET_ROOT of an attached node.
  RootAttached { op: usize, id: u32 },
  /// INSERT whose `before` is not another child of the parent.
  BadBefore { op: usize, parent: u32, before: u32 },
  /// REMOVE of a child that is not attached to that parent.
  NotAChild { op: usize, parent: u32, child: u32 },
}

impl ApplyError {
  /// The index of the operation at fault.
  pub fn op(&self) -> usize {
    match self {
      ApplyError::IdInUse { op, .. }
      | ApplyError::UnknownNode { op, .. }
      | ApplyError::WrongKind { op, .. }
      | ApplyError::Cycle { op, .. }
      | ApplyError::RootAttached { op, .. }
      | ApplyError::BadBefore { op, .. }
      | ApplyError::NotAChild { op, .. } => *op,
    }
  }
}

impl std::fmt::Display for ApplyError {
  fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
    match self {
      ApplyError::IdInUse { op, id } => {
        write!(f, "op {op}: node {id} already exists")
      }
      ApplyError::UnknownNode { op, id } => {
        write!(f, "op {op}: no node {id}")
      }
      ApplyError::WrongKind { op, id, kind } => {
        write!(f, "op {op}: node {id} is a {}", kind.name())
      }
      ApplyError::Cycle { op, parent, child } => {
        write!(f, "op {op}: node {child} contains node {parent}")
      }
      ApplyError::RootAttached { op, id } => {
        write!(f, "op {op}: node {id} cannot be both the root and a child")
      }
      ApplyError::BadBefore { op, parent, before } => {
        write!(f, "op {op}: node {before} is not another child of {parent}")
      }
      ApplyError::NotAChild { op, parent, child } => {
        write!(f, "op {op}: node {child} is not a child of {parent}")
      }
    }
  }
}

impl std::error::Error for ApplyError {}

/// An image operation of an applied batch, for the host's image cache.
#[derive(Debug, Clone, PartialEq)]
pub enum ImageOp {
  Data {
    image_id: u32,
    mime: String,
    bytes: Vec<u8>,
  },
  Free {
    image_id: u32,
  },
}

/// What an applied batch did that a host keeps state for.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Applied {
  /// Nodes created, in order.
  pub created: Vec<u32>,
  /// Nodes freed (each DESTROY's whole subtree).
  pub destroyed: Vec<u32>,
  /// Nodes whose non-style props changed, once each, in first-change order.
  pub props_changed: Vec<u32>,
  /// Image nodes whose image id changed.
  pub images_set: Vec<u32>,
  /// The image operations, in order.
  pub images: Vec<ImageOp>,
}

/// The retained tree of one window.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Tree {
  nodes: HashMap<u32, Node>,
  root: u32,
  seq: u32,
}

/// What a batch would change, while it is being checked.
struct Overlay<'a> {
  tree: &'a Tree,
  created: HashMap<u32, Kind>,
  /// Parents the batch set (`None`: detached).
  parents: HashMap<u32, Option<u32>>,
  /// The roots of the subtrees the batch destroyed.
  destroyed: HashSet<u32>,
  root: u32,
}

impl Overlay<'_> {
  fn parent(&self, id: u32) -> Option<u32> {
    match self.parents.get(&id) {
      Some(parent) => *parent,
      None => self.tree.nodes.get(&id).and_then(|node| node.parent),
    }
  }

  /// The kind of `id`, destroyed or not.
  fn known(&self, id: u32) -> Option<Kind> {
    self
      .created
      .get(&id)
      .copied()
      .or_else(|| self.tree.nodes.get(&id).map(|node| node.kind))
  }

  /// The kind of `id` when it is alive: known, and neither it nor an ancestor was destroyed.
  /// (A destroyed subtree keeps its parent links here, so its nodes stay dead.)
  fn alive(&self, id: u32) -> Option<Kind> {
    let kind = self.known(id)?;
    let mut at = id;
    loop {
      if self.destroyed.contains(&at) {
        return None;
      }
      match self.parent(at) {
        Some(parent) => at = parent,
        None => return Some(kind),
      }
    }
  }

  fn node(&self, op: usize, id: u32) -> Result<Kind, ApplyError> {
    self.alive(id).ok_or(ApplyError::UnknownNode { op, id })
  }

  /// Whether `ancestor` is `id` or above it.
  fn contains(&self, ancestor: u32, id: u32) -> bool {
    let mut at = Some(id);
    while let Some(node) = at {
      if node == ancestor {
        return true;
      }
      at = self.parent(node);
    }
    false
  }

  fn check(&mut self, index: usize, op: &Op) -> Result<(), ApplyError> {
    match op {
      Op::Create { id, kind } => {
        if self.known(*id).is_some() {
          return Err(ApplyError::IdInUse { op: index, id: *id });
        }
        self.created.insert(*id, *kind);
      }
      Op::Insert {
        parent,
        child,
        before,
      } => {
        let (parent, child, before) = (*parent, *child, *before);
        let kind = self.node(index, parent)?;
        if !takes_children(kind) {
          return Err(ApplyError::WrongKind {
            op: index,
            id: parent,
            kind,
          });
        }
        self.node(index, child)?;
        if self.contains(child, parent) {
          return Err(ApplyError::Cycle {
            op: index,
            parent,
            child,
          });
        }
        if child == self.root {
          return Err(ApplyError::RootAttached {
            op: index,
            id: child,
          });
        }
        if before != 0
          && (before == child
            || self.alive(before).is_none()
            || self.parent(before) != Some(parent))
        {
          return Err(ApplyError::BadBefore {
            op: index,
            parent,
            before,
          });
        }
        self.parents.insert(child, Some(parent));
      }
      Op::Remove { parent, child } => {
        self.node(index, *parent)?;
        self.node(index, *child)?;
        if self.parent(*child) != Some(*parent) {
          return Err(ApplyError::NotAChild {
            op: index,
            parent: *parent,
            child: *child,
          });
        }
        self.parents.insert(*child, None);
      }
      Op::Destroy { id } => {
        self.node(index, *id)?;
        if self.root != 0 && self.contains(*id, self.root) {
          self.root = 0;
        }
        self.destroyed.insert(*id);
      }
      Op::SetRoot { id } => {
        if *id != 0 {
          self.node(index, *id)?;
          if self.parent(*id).is_some() {
            return Err(ApplyError::RootAttached { op: index, id: *id });
          }
        }
        self.root = *id;
      }
      Op::SetStyle { id, .. }
      | Op::SetProp { id, .. }
      | Op::Listen { id, .. } => {
        self.node(index, *id)?;
      }
      Op::SetText { id, .. } => {
        let kind = self.node(index, *id)?;
        if kind != Kind::RawText {
          return Err(ApplyError::WrongKind {
            op: index,
            id: *id,
            kind,
          });
        }
      }
      Op::SetImage { id, .. } => {
        let kind = self.node(index, *id)?;
        if kind != Kind::Image {
          return Err(ApplyError::WrongKind {
            op: index,
            id: *id,
            kind,
          });
        }
      }
      Op::ImageData { .. } | Op::ImageFree { .. } => {}
    }
    Ok(())
  }
}

impl Tree {
  pub fn new() -> Self {
    Self::default()
  }

  /// The root's id, 0 for none.
  pub fn root(&self) -> u32 {
    self.root
  }

  /// The `seq` of the last applied batch.
  pub fn seq(&self) -> u32 {
    self.seq
  }

  pub fn get(&self, id: u32) -> Option<&Node> {
    self.nodes.get(&id)
  }

  pub fn len(&self) -> usize {
    self.nodes.len()
  }

  pub fn is_empty(&self) -> bool {
    self.nodes.is_empty()
  }

  /// Every node, in no order.
  pub fn nodes(&self) -> impl Iterator<Item = &Node> {
    self.nodes.values()
  }

  /// The text of `id`'s RawText descendants (and its own), in order.
  pub fn text_content(&self, id: u32) -> String {
    let mut out = String::new();
    self.collect_text(id, &mut out);
    out
  }

  fn collect_text(&self, id: u32, out: &mut String) {
    let Some(node) = self.nodes.get(&id) else {
      return;
    };
    out.push_str(&node.text);
    for child in &node.children {
      self.collect_text(*child, out);
    }
  }

  /// Check `batch` against the tree without changing it.
  pub fn validate(&self, batch: &Batch) -> Result<(), ApplyError> {
    let mut overlay = Overlay {
      tree: self,
      created: HashMap::new(),
      parents: HashMap::new(),
      destroyed: HashSet::new(),
      root: self.root,
    };
    for (index, op) in batch.ops.iter().enumerate() {
      overlay.check(index, op)?;
    }
    Ok(())
  }

  /// Apply `batch` whole, or not at all.
  pub fn apply(&mut self, batch: Batch) -> Result<Applied, ApplyError> {
    self.validate(&batch)?;
    let mut applied = Applied::default();
    let mut props_seen = HashSet::new();
    for op in batch.ops {
      self.apply_op(op, &mut applied, &mut props_seen);
    }
    self.seq = batch.seq;
    Ok(applied)
  }

  fn detach(&mut self, child: u32) {
    let parent = self
      .nodes
      .get_mut(&child)
      .and_then(|node| node.parent.take());
    if let Some(parent) = parent.and_then(|id| self.nodes.get_mut(&id)) {
      parent.children.retain(|id| *id != child);
    }
  }

  /// One operation of a batch [`Tree::validate`] accepted.
  fn apply_op(
    &mut self,
    op: Op,
    applied: &mut Applied,
    props_seen: &mut HashSet<u32>,
  ) {
    match op {
      Op::Create { id, kind } => {
        self.nodes.insert(id, Node::new(id, kind));
        applied.created.push(id);
      }
      Op::Insert {
        parent,
        child,
        before,
      } => {
        self.detach(child);
        if let Some(node) = self.nodes.get_mut(&child) {
          node.parent = Some(parent);
        }
        if let Some(node) = self.nodes.get_mut(&parent) {
          let at = node
            .children
            .iter()
            .position(|id| *id == before)
            .unwrap_or(node.children.len());
          node.children.insert(at, child);
        }
      }
      Op::Remove { child, .. } => self.detach(child),
      Op::Destroy { id } => {
        self.detach(id);
        let mut pending = vec![id];
        while let Some(id) = pending.pop() {
          if let Some(node) = self.nodes.remove(&id) {
            pending.extend(node.children);
            applied.destroyed.push(id);
            if self.root == id {
              self.root = 0;
            }
          }
        }
      }
      Op::SetRoot { id } => self.root = id,
      Op::SetStyle { id, props } => {
        if let Some(node) = self.nodes.get_mut(&id) {
          for (prop, value) in props {
            if value == Value::Unset {
              node.style.remove(&prop);
            } else {
              node.style.insert(prop, value);
            }
          }
        }
      }
      Op::SetProp { id, prop, value } => {
        if let Some(node) = self.nodes.get_mut(&id) {
          if value == Value::Unset {
            node.props.remove(&prop);
          } else {
            node.props.insert(prop, value);
          }
          if props_seen.insert(id) {
            applied.props_changed.push(id);
          }
        }
      }
      Op::SetText { id, text } => {
        if let Some(node) = self.nodes.get_mut(&id) {
          node.text = text;
        }
      }
      Op::Listen { id, mask } => {
        if let Some(node) = self.nodes.get_mut(&id) {
          node.listen = mask;
        }
      }
      Op::SetImage { id, image_id } => {
        if let Some(node) = self.nodes.get_mut(&id) {
          node.image = image_id;
          applied.images_set.push(id);
        }
      }
      Op::ImageData {
        image_id,
        mime,
        bytes,
      } => {
        applied.images.push(ImageOp::Data {
          image_id,
          mime,
          bytes,
        });
      }
      Op::ImageFree { image_id } => {
        applied.images.push(ImageOp::Free { image_id });
      }
    }
  }
}
