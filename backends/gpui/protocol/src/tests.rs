//! Unit tests: the id tables, the codec and the tree. Run in CI (the train), not locally.

use crate::ids::enums;
use crate::*;

fn batch(seq: u32, build: impl FnOnce(&mut BatchBuilder)) -> Vec<u8> {
  let mut b = BatchBuilder::new(seq);
  build(&mut b);
  b.finish()
}

fn decoded(seq: u32, build: impl FnOnce(&mut BatchBuilder)) -> Batch {
  decode(&batch(seq, build)).expect("the batch decodes")
}

/// A tree with a root View 1 holding Text 2 (RawText 3 "hi") and Pressable 4.
fn small_tree() -> Tree {
  let mut tree = Tree::new();
  tree
    .apply(decoded(1, |b| {
      b.create(1, Kind::View)
        .create(2, Kind::Text)
        .create(3, Kind::RawText)
        .create(4, Kind::Pressable)
        .set_text(3, "hi")
        .insert(2, 3, 0)
        .insert(1, 2, 0)
        .insert(1, 4, 0)
        .set_root(1);
    }))
    .expect("the first batch applies");
  tree
}

// --- The id tables ----------------------------------------------------------------------

#[test]
fn the_table_has_the_ids_of_props_json() {
  // Spot checks against docs/b1-props.json: a renumbering shows here.
  assert_eq!(ids::VERSION, 1);
  assert!(ids::SOURCE.contains("b1-props.json"));
  assert_eq!(Kind::View as u16, 1);
  assert_eq!(Kind::Pressable as u16, 7);
  assert_eq!(Kind::ALL.len(), 7);
  assert_eq!(StyleProp::Display as u16, 1);
  assert_eq!(StyleProp::Width as u16, 20);
  assert_eq!(StyleProp::BackgroundColor as u16, 40);
  assert_eq!(StyleProp::TextDecorationLine as u16, 58);
  assert_eq!(StyleProp::ALL.len(), 58);
  assert_eq!(Prop::Value as u16, 1);
  assert_eq!(Prop::TestId as u16, 13);
  assert_eq!(Prop::PlaceholderTextColor as u16, 14);
  assert_eq!(Prop::ALL.len(), 14);
  assert_eq!(enums::flex_direction::COLUMN, 1);
  assert_eq!(enums::justify_content::SPACE_EVENLY, 5);
  assert_eq!(enums::align_items::STRETCH, 4);
  assert_eq!(enums::text_decoration_line::UNDERLINE_LINE_THROUGH, 3);
  assert_eq!(enums::resize_mode::CENTER, 4);
  assert_eq!(listen::PRESS, 1);
  assert_eq!(listen::IMAGE, 64);
  assert_eq!(listen::ALL, 127);
}

#[test]
fn ids_round_trip_and_have_names() {
  for kind in Kind::ALL {
    assert_eq!(Kind::from_u16(*kind as u16), Some(*kind));
  }
  for prop in StyleProp::ALL {
    assert_eq!(StyleProp::from_u16(*prop as u16), Some(*prop));
    assert!(!prop.name().is_empty());
    // Every style prop has a class (and an enum-valued one its table).
    let _ = prop.class();
  }
  for prop in Prop::ALL {
    assert_eq!(Prop::from_u16(*prop as u16), Some(*prop));
    let _ = prop.class();
  }
  assert_eq!(Kind::from_u16(0), None);
  assert_eq!(Kind::from_u16(8), None);
  assert_eq!(StyleProp::from_u16(0), None);
  assert_eq!(StyleProp::from_u16(59), None);
  assert_eq!(Prop::from_u16(15), None);
  assert_eq!(StyleProp::FlexDirection.name(), "flexDirection");
  assert_eq!(Prop::TestId.name(), "testID");
  assert_eq!(
    StyleProp::FlexDirection.enum_values(),
    Some(enums::flex_direction::VALUES)
  );
  assert_eq!(StyleProp::Width.enum_values(), None);
  assert_eq!(
    Prop::ResizeMode.enum_values(),
    Some(enums::resize_mode::VALUES)
  );
  assert_eq!(
    enums::flex_direction::NAMES,
    &[
      ("row", 0),
      ("column", 1),
      ("row-reverse", 2),
      ("column-reverse", 3)
    ]
  );
}

#[test]
fn value_classes_take_what_they_should() {
  let length = StyleProp::Width.class();
  assert!(length.accepts(&Value::F32(10.0)));
  assert!(length.accepts(&Value::Percent(50.0)));
  assert!(length.accepts(&Value::Auto));
  assert!(length.accepts(&Value::Unset));
  assert!(!length.accepts(&Value::F32(f32::NAN)));
  assert!(!length.accepts(&Value::Color(0)));
  let padding = StyleProp::PaddingTop.class();
  assert!(padding.accepts(&Value::Percent(5.0)));
  assert!(!padding.accepts(&Value::Auto));
  let number = StyleProp::Opacity.class();
  assert!(number.accepts(&Value::F32(0.5)));
  assert!(number.accepts(&Value::F64(0.5)));
  assert!(!number.accepts(&Value::F64(f64::INFINITY)));
  assert!(!number.accepts(&Value::Percent(50.0)));
  let direction = StyleProp::FlexDirection.class();
  assert!(direction.accepts(&Value::Enum(3)));
  assert!(!direction.accepts(&Value::Enum(4)));
  assert!(!direction.accepts(&Value::F32(1.0)));
  assert!(StyleProp::Color.class().accepts(&Value::Color(0xff0000ff)));
  assert!(
    StyleProp::FontFamily
      .class()
      .accepts(&Value::String("Inter".into()))
  );
  assert!(Prop::Editable.class().accepts(&Value::Bool(true)));
  assert!(!Prop::Editable.class().accepts(&Value::F32(1.0)));
  assert!(Prop::EventCount.class().accepts(&Value::F64(1e9)));
  assert!(Prop::PlaceholderTextColor.class().accepts(&Value::Color(1)));
  assert!(Prop::Value.class().accepts(&Value::Unset));
}

// --- The codec --------------------------------------------------------------------------

#[test]
fn the_header_is_laid_out_as_documented() {
  let bytes = batch(0x01020304, |b| {
    b.create(9, Kind::Text);
  });
  assert_eq!(&bytes[0..4], b"NTB1");
  assert_eq!(&bytes[4..6], &[1, 0]); // version
  assert_eq!(&bytes[6..8], &[0, 0]); // flags
  assert_eq!(&bytes[8..12], &[4, 3, 2, 1]); // seq, little-endian
  assert_eq!(&bytes[12..16], &[1, 0, 0, 0]); // op_count
  assert_eq!(&bytes[16..], &[1, 9, 0, 0, 0, 2, 0]); // CREATE id=9 kind=2
}

#[test]
fn every_op_round_trips() {
  let ops = vec![
    Op::Create {
      id: 1,
      kind: Kind::View,
    },
    Op::Create {
      id: 2,
      kind: Kind::Image,
    },
    Op::Insert {
      parent: 1,
      child: 2,
      before: 0,
    },
    Op::Remove {
      parent: 1,
      child: 2,
    },
    Op::Destroy { id: 2 },
    Op::SetRoot { id: 1 },
    Op::SetRoot { id: 0 },
    Op::SetStyle {
      id: 1,
      props: vec![
        (StyleProp::Width, Value::F32(120.5)),
        (StyleProp::Height, Value::Percent(50.0)),
        (StyleProp::FlexBasis, Value::Auto),
        (
          StyleProp::FlexDirection,
          Value::Enum(enums::flex_direction::ROW),
        ),
        (StyleProp::BackgroundColor, Value::Color(0x11223344)),
        (StyleProp::FontFamily, Value::String("Zed Sans é".into())),
        (StyleProp::Opacity, Value::F64(0.25)),
        (StyleProp::MarginTop, Value::Unset),
      ],
    },
    Op::SetStyle {
      id: 1,
      props: vec![],
    },
    Op::SetProp {
      id: 1,
      prop: Prop::Disabled,
      value: Value::Bool(true),
    },
    Op::SetProp {
      id: 1,
      prop: Prop::EventCount,
      value: Value::F64(7.0),
    },
    Op::SetProp {
      id: 1,
      prop: Prop::Value,
      value: Value::String("".into()),
    },
    Op::SetText {
      id: 3,
      text: "héllo 🙂".into(),
    },
    Op::Listen {
      id: 1,
      mask: listen::PRESS | listen::LAYOUT,
    },
    Op::SetImage { id: 2, image_id: 5 },
    Op::SetImage { id: 2, image_id: 0 },
    Op::ImageData {
      image_id: 5,
      mime: "image/png".into(),
      bytes: vec![0x89, b'P', b'N', b'G', 0, 255],
    },
    Op::ImageFree { image_id: 5 },
  ];
  let mut b = BatchBuilder::new(42);
  for op in &ops {
    b.push(op);
  }
  assert_eq!(b.len() as usize, ops.len());
  let got = decode(&b.finish()).expect("decodes");
  assert_eq!(got, Batch { seq: 42, ops });
}

#[test]
fn an_empty_batch_is_a_batch() {
  let b = BatchBuilder::new(3);
  assert!(b.is_empty());
  assert_eq!(
    decode(&b.finish()),
    Ok(Batch {
      seq: 3,
      ops: vec![]
    })
  );
}

#[test]
fn a_bad_header_is_refused() {
  assert_eq!(decode(b""), Err(DecodeError::BadMagic));
  assert_eq!(decode(b"NTB"), Err(DecodeError::BadMagic));
  assert_eq!(
    decode(b"NTB2\x01\0\0\0\0\0\0\0\0\0\0\0"),
    Err(DecodeError::BadMagic)
  );
  let good = BatchBuilder::new(1).finish();
  let mut version = good.clone();
  version[4] = 2;
  assert_eq!(decode(&version), Err(DecodeError::UnsupportedVersion(2)));
  let mut flags = good.clone();
  flags[6] = 1;
  assert_eq!(decode(&flags), Err(DecodeError::UnsupportedFlags(1)));
  assert_eq!(decode(&good[..10]), Err(DecodeError::Truncated { at: 8 }));
}

#[test]
fn the_op_count_must_match_the_bytes() {
  let good = batch(1, |b| {
    b.create(1, Kind::View).create(2, Kind::View);
  });
  // One more than there is.
  let mut more = good.clone();
  more[12] = 3;
  assert!(matches!(decode(&more), Err(DecodeError::Truncated { .. })));
  // One fewer: the second operation is trailing bytes.
  let mut fewer = good.clone();
  fewer[12] = 1;
  assert_eq!(decode(&fewer), Err(DecodeError::TrailingBytes { at: 23 }));
  // A count far beyond the bytes neither allocates nor decodes.
  let mut huge = good.clone();
  huge[12..16].copy_from_slice(&u32::MAX.to_le_bytes());
  assert!(matches!(decode(&huge), Err(DecodeError::Truncated { .. })));
  // Cut anywhere, a batch never decodes.
  for len in 0..good.len() {
    assert!(decode(&good[..len]).is_err(), "decoded at {len} bytes");
  }
}

#[test]
fn unknown_ids_and_bad_values_are_refused() {
  let raw = |tail: &[u8]| {
    let mut bytes = BatchBuilder::new(1).finish();
    bytes[12] = 1;
    bytes.extend_from_slice(tail);
    decode(&bytes)
  };
  assert_eq!(
    raw(&[0]),
    Err(DecodeError::UnknownOpcode { at: 16, opcode: 0 })
  );
  assert_eq!(
    raw(&[13]),
    Err(DecodeError::UnknownOpcode { at: 16, opcode: 13 })
  );
  // CREATE id=0, then an unknown kind.
  assert_eq!(
    raw(&[1, 0, 0, 0, 0, 1, 0]),
    Err(DecodeError::ZeroId { at: 17 })
  );
  assert_eq!(
    raw(&[1, 1, 0, 0, 0, 99, 0]),
    Err(DecodeError::UnknownKind { at: 21, kind: 99 })
  );
  // INSERT with parent 0 / child 0.
  assert!(matches!(
    raw(&[2, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0]),
    Err(DecodeError::ZeroId { .. })
  ));
  assert!(matches!(
    raw(&[2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    Err(DecodeError::ZeroId { .. })
  ));
  // SET_STYLE node=1 n=1 prop=999.
  assert_eq!(
    raw(&[6, 1, 0, 0, 0, 1, 0, 0xe7, 0x03, 0]),
    Err(DecodeError::UnknownStyleProp { at: 23, prop: 999 })
  );
  // SET_STYLE width = tag 9.
  assert_eq!(
    raw(&[6, 1, 0, 0, 0, 1, 0, 20, 0, 9]),
    Err(DecodeError::UnknownValueTag { at: 25, tag: 9 })
  );
  // SET_STYLE width = color: not a value width takes.
  assert_eq!(
    raw(&[6, 1, 0, 0, 0, 1, 0, 20, 0, 5, 0, 0, 0, 0]),
    Err(DecodeError::BadValue {
      at: 25,
      prop: "width"
    })
  );
  // SET_STYLE flexDirection = enum 9.
  assert_eq!(
    raw(&[6, 1, 0, 0, 0, 1, 0, 2, 0, 4, 9]),
    Err(DecodeError::BadValue {
      at: 25,
      prop: "flexDirection"
    })
  );
  // SET_PROP unknown prop, and a bool of 2.
  assert_eq!(
    raw(&[7, 1, 0, 0, 0, 200, 0, 0]),
    Err(DecodeError::UnknownProp { at: 21, prop: 200 })
  );
  assert_eq!(
    raw(&[7, 1, 0, 0, 0, 10, 0, 7, 2]),
    Err(DecodeError::BadValue {
      at: 23,
      prop: "bool"
    })
  );
  // SET_TEXT with invalid UTF-8, and a length past the end.
  assert_eq!(
    raw(&[8, 1, 0, 0, 0, 1, 0, 0, 0, 0xff]),
    Err(DecodeError::BadUtf8 { at: 21 })
  );
  assert!(matches!(
    raw(&[8, 1, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, b'a']),
    Err(DecodeError::Truncated { .. })
  ));
  // LISTEN with a bit outside the table.
  assert_eq!(
    raw(&[9, 1, 0, 0, 0, 0x80, 0, 0, 0]),
    Err(DecodeError::BadMask { at: 21, mask: 0x80 })
  );
  // IMAGE_DATA / IMAGE_FREE of image 0.
  assert!(matches!(
    raw(&[11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]),
    Err(DecodeError::ZeroId { .. })
  ));
  assert!(matches!(
    raw(&[12, 0, 0, 0, 0]),
    Err(DecodeError::ZeroId { .. })
  ));
  // A NaN length.
  let nan = batch(1, |b| {
    b.set_style(1, &[(StyleProp::Width, Value::F32(f32::NAN))]);
  });
  assert!(matches!(decode(&nan), Err(DecodeError::BadValue { .. })));
}

// --- The tree ---------------------------------------------------------------------------

#[test]
fn a_batch_builds_the_tree() {
  let tree = small_tree();
  assert_eq!(tree.root(), 1);
  assert_eq!(tree.seq(), 1);
  assert_eq!(tree.len(), 4);
  assert_eq!(tree.get(1).unwrap().children, vec![2, 4]);
  assert_eq!(tree.get(2).unwrap().parent, Some(1));
  assert_eq!(tree.get(3).unwrap().text, "hi");
  assert_eq!(tree.text_content(1), "hi");
  assert_eq!(tree.get(1).unwrap().kind, Kind::View);
}

#[test]
fn insert_orders_and_moves() {
  let mut tree = small_tree();
  let applied = tree
    .apply(decoded(2, |b| {
      b.create(5, Kind::View)
        .create(6, Kind::View)
        .insert(1, 5, 4) // before the pressable
        .insert(1, 6, 2) // first
        .insert(1, 4, 6) // move the pressable to the front
        .insert(5, 2, 0); // reparent the text under 5
    }))
    .unwrap();
  assert_eq!(applied.created, vec![5, 6]);
  assert_eq!(tree.get(1).unwrap().children, vec![4, 6, 5]);
  assert_eq!(tree.get(5).unwrap().children, vec![2]);
  assert_eq!(tree.get(2).unwrap().parent, Some(5));
  assert_eq!(tree.seq(), 2);
}

#[test]
fn remove_detaches_and_destroy_frees_the_subtree() {
  let mut tree = small_tree();
  tree
    .apply(decoded(2, |b| {
      b.remove(1, 2);
    }))
    .unwrap();
  assert_eq!(tree.get(1).unwrap().children, vec![4]);
  assert_eq!(tree.get(2).unwrap().parent, None);
  assert_eq!(tree.get(2).unwrap().children, vec![3]);
  let applied = tree
    .apply(decoded(3, |b| {
      b.destroy(2);
    }))
    .unwrap();
  let mut destroyed = applied.destroyed.clone();
  destroyed.sort();
  assert_eq!(destroyed, vec![2, 3]);
  assert!(tree.get(2).is_none() && tree.get(3).is_none());
  // Destroying an attached node detaches it; destroying the root clears it.
  let applied = tree
    .apply(decoded(4, |b| {
      b.destroy(4).destroy(1);
    }))
    .unwrap();
  assert_eq!(applied.destroyed, vec![4, 1]);
  assert_eq!(tree.root(), 0);
  assert!(tree.is_empty());
}

#[test]
fn styles_props_text_and_masks_are_kept() {
  let mut tree = small_tree();
  let applied = tree
    .apply(decoded(2, |b| {
      b.set_style(
        1,
        &[
          (StyleProp::Width, Value::F32(100.0)),
          (StyleProp::Opacity, Value::F32(0.5)),
        ],
      )
      .set_style(1, &[(StyleProp::Opacity, Value::Unset)])
      .set_prop(4, Prop::Disabled, &Value::Bool(true))
      .set_prop(4, Prop::TestId, &Value::String("button".into()))
      .set_prop(4, Prop::TestId, &Value::Unset)
      .set_text(3, "bye")
      .listen(4, listen::PRESS)
      .listen(4, listen::PRESS | listen::LAYOUT);
    }))
    .unwrap();
  let root = tree.get(1).unwrap();
  assert_eq!(root.style(StyleProp::Width), Some(&Value::F32(100.0)));
  assert_eq!(root.style(StyleProp::Opacity), None);
  let button = tree.get(4).unwrap();
  assert_eq!(button.prop(Prop::Disabled), Some(&Value::Bool(true)));
  assert_eq!(button.prop(Prop::TestId), None);
  assert!(button.listens(listen::LAYOUT) && !button.listens(listen::SCROLL));
  assert_eq!(applied.props_changed, vec![4]);
  assert_eq!(tree.text_content(1), "bye");
}

#[test]
fn image_ops_are_handed_to_the_host() {
  let mut tree = small_tree();
  let applied = tree
    .apply(decoded(2, |b| {
      b.create(5, Kind::Image)
        .insert(1, 5, 0)
        .image_data(9, "image/png", &[1, 2, 3])
        .set_image(5, 9)
        .image_free(8);
    }))
    .unwrap();
  assert_eq!(tree.get(5).unwrap().image, 9);
  assert_eq!(applied.images_set, vec![5]);
  assert_eq!(
    applied.images,
    vec![
      ImageOp::Data {
        image_id: 9,
        mime: "image/png".into(),
        bytes: vec![1, 2, 3]
      },
      ImageOp::Free { image_id: 8 },
    ]
  );
}

/// Every fault leaves the tree exactly as it was, whatever came before it in the batch.
#[test]
#[allow(clippy::type_complexity)]
fn a_batch_that_does_not_fit_changes_nothing() {
  let cases: Vec<(&str, Box<dyn Fn(&mut BatchBuilder)>, ApplyError)> = vec![
    (
      "create an id in use",
      Box::new(|b| {
        b.create(4, Kind::View);
      }),
      ApplyError::IdInUse { op: 2, id: 4 },
    ),
    (
      "create an id twice",
      Box::new(|b| {
        b.create(8, Kind::View).create(8, Kind::View);
      }),
      ApplyError::IdInUse { op: 3, id: 8 },
    ),
    (
      "reuse an id destroyed in the batch",
      Box::new(|b| {
        b.destroy(4).create(4, Kind::View);
      }),
      ApplyError::IdInUse { op: 3, id: 4 },
    ),
    (
      "insert under an unknown parent",
      Box::new(|b| {
        b.insert(77, 4, 0);
      }),
      ApplyError::UnknownNode { op: 2, id: 77 },
    ),
    (
      "insert an unknown child",
      Box::new(|b| {
        b.insert(1, 77, 0);
      }),
      ApplyError::UnknownNode { op: 2, id: 77 },
    ),
    (
      "insert under a RawText",
      Box::new(|b| {
        b.insert(3, 4, 0);
      }),
      ApplyError::WrongKind {
        op: 2,
        id: 3,
        kind: Kind::RawText,
      },
    ),
    (
      "insert a node under itself",
      Box::new(|b| {
        b.insert(4, 4, 0);
      }),
      ApplyError::Cycle {
        op: 2,
        parent: 4,
        child: 4,
      },
    ),
    (
      "insert a node under its descendant",
      Box::new(|b| {
        b.create(8, Kind::View)
          .insert(4, 8, 0)
          .remove(1, 4)
          .insert(8, 4, 0);
      }),
      ApplyError::Cycle {
        op: 5,
        parent: 8,
        child: 4,
      },
    ),
    (
      "insert the root",
      Box::new(|b| {
        b.insert(4, 1, 0);
      }),
      ApplyError::Cycle {
        op: 2,
        parent: 4,
        child: 1,
      },
    ),
    (
      "insert the root under a detached node",
      Box::new(|b| {
        b.create(8, Kind::View).insert(8, 1, 0);
      }),
      ApplyError::RootAttached { op: 3, id: 1 },
    ),
    (
      "insert before a node of another parent",
      Box::new(|b| {
        b.insert(1, 9, 3);
      }),
      ApplyError::BadBefore {
        op: 2,
        parent: 1,
        before: 3,
      },
    ),
    (
      "insert before itself",
      Box::new(|b| {
        b.insert(1, 4, 4);
      }),
      ApplyError::BadBefore {
        op: 2,
        parent: 1,
        before: 4,
      },
    ),
    (
      "remove from the wrong parent",
      Box::new(|b| {
        b.remove(1, 3);
      }),
      ApplyError::NotAChild {
        op: 2,
        parent: 1,
        child: 3,
      },
    ),
    (
      "remove twice",
      Box::new(|b| {
        b.remove(1, 4).remove(1, 4);
      }),
      ApplyError::NotAChild {
        op: 3,
        parent: 1,
        child: 4,
      },
    ),
    (
      "destroy an unknown node",
      Box::new(|b| {
        b.destroy(77);
      }),
      ApplyError::UnknownNode { op: 2, id: 77 },
    ),
    (
      "use a node destroyed with its parent",
      Box::new(|b| {
        b.destroy(2).set_text(3, "gone");
      }),
      ApplyError::UnknownNode { op: 3, id: 3 },
    ),
    (
      "destroy twice",
      Box::new(|b| {
        b.destroy(4).destroy(4);
      }),
      ApplyError::UnknownNode { op: 3, id: 4 },
    ),
    (
      "root an attached node",
      Box::new(|b| {
        b.set_root(4);
      }),
      ApplyError::RootAttached { op: 2, id: 4 },
    ),
    (
      "root an unknown node",
      Box::new(|b| {
        b.set_root(77);
      }),
      ApplyError::UnknownNode { op: 2, id: 77 },
    ),
    (
      "style an unknown node",
      Box::new(|b| {
        b.set_style(77, &[(StyleProp::Width, Value::F32(1.0))]);
      }),
      ApplyError::UnknownNode { op: 2, id: 77 },
    ),
    (
      "set a prop of an unknown node",
      Box::new(|b| {
        b.set_prop(77, Prop::Disabled, &Value::Bool(true));
      }),
      ApplyError::UnknownNode { op: 2, id: 77 },
    ),
    (
      "listen on an unknown node",
      Box::new(|b| {
        b.listen(77, listen::PRESS);
      }),
      ApplyError::UnknownNode { op: 2, id: 77 },
    ),
    (
      "set the text of a View",
      Box::new(|b| {
        b.set_text(1, "no");
      }),
      ApplyError::WrongKind {
        op: 2,
        id: 1,
        kind: Kind::View,
      },
    ),
    (
      "set the image of a View",
      Box::new(|b| {
        b.set_image(1, 5);
      }),
      ApplyError::WrongKind {
        op: 2,
        id: 1,
        kind: Kind::View,
      },
    ),
  ];
  for (name, fault, expected) in cases {
    let mut tree = small_tree();
    let before = tree.clone();
    // Two good operations first: they must not survive the fault either.
    let bytes = batch(9, |b| {
      b.create(9, Kind::View).set_text(3, "changed");
      fault(b);
    });
    let batch = decode(&bytes).unwrap_or_else(|e| panic!("{name}: {e}"));
    let error = tree.apply(batch).expect_err(name);
    assert_eq!(error, expected, "{name}");
    assert_eq!(error.op(), expected.op());
    assert!(!error.to_string().is_empty());
    assert_eq!(tree, before, "{name}: the tree changed");
  }
}

#[test]
fn what_one_batch_creates_it_can_use() {
  let mut tree = Tree::new();
  tree
    .apply(decoded(1, |b| {
      b.create(1, Kind::ScrollView)
        .create(2, Kind::View)
        .insert(1, 2, 0)
        .remove(1, 2)
        .insert(1, 2, 0)
        .set_root(1)
        .set_root(0)
        .set_root(1);
    }))
    .unwrap();
  assert_eq!(tree.root(), 1);
  assert_eq!(tree.get(1).unwrap().children, vec![2]);
  // A detached child survives its old parent.
  tree
    .apply(decoded(2, |b| {
      b.remove(1, 2).destroy(1).set_root(2);
    }))
    .unwrap();
  assert_eq!(tree.root(), 2);
  assert_eq!(tree.len(), 1);
}
