# Native tree

A window can host a **native tree** (API ≥ 61): a retained tree of native
primitives (views, text, images, scroll views, text inputs, pressables) that the
runtime mutates in batches. It is what a renderer with no web page draws through
(a React Native-style reconciler, for one): the runtime sends mutations, the
backend lays the tree out and paints it with its own UI toolkit, and input comes
back as events.

The capability is generic. A backend that has a renderer for it fills the
`native_tree_*` entries of the [API table](c-abi.md) and reports
`LAUFEY_WINDOW_CAP_NATIVE_TREE`; every other backend leaves them `NULL`.

| Backend | Native tree                                                        |
| ------- | ------------------------------------------------------------------ |
| GPUI    | version 1 ([`backends/gpui`](backends.md#gpui))                    |
| CEF     | no (`NULL`; `NativeTree::commit` answers `Unsupported`)            |
| WebView | no (`NULL`)                                                        |
| Winit   | no (`NULL`); a Winit host with a renderer of its own could add one |

## The API

```c
#define LAUFEY_WINDOW_CAP_NATIVE_TREE (1u << 27)
#define LAUFEY_NATIVE_TREE_OK 0
#define LAUFEY_NATIVE_TREE_UNSUPPORTED (-1)
#define LAUFEY_NATIVE_TREE_MALFORMED (-2)
#define LAUFEY_NATIVE_TREE_NO_WINDOW (-3)

typedef void (*laufey_native_tree_event_fn)(void* user_data, uint32_t window_id,
                                            const char* json, size_t len);

uint32_t (*native_tree_version)(void* backend_data);
int32_t (*native_tree_commit)(void* backend_data, uint32_t window_id,
                              const uint8_t* data, size_t len);
void (*set_native_tree_event_handler)(void* backend_data,
                                      laufey_native_tree_event_fn handler,
                                      void* user_data);
char* (*native_tree_snapshot)(void* backend_data, uint32_t window_id);
```

- `native_tree_version`: the batch format the backend takes (1), 0 for none.
- `native_tree_commit`: one batch for one window. The backend copies the bytes
  before it returns and applies the whole batch **atomically on the UI thread**
  before the next frame; the batches of a window apply in commit order. Any
  thread.
- `set_native_tree_event_handler`: one process-wide handler for the events of
  every window, called on the UI thread with one JSON object.
- `native_tree_snapshot`: the tree read back as JSON, for self-tests (free with
  `string_free`).

In the `laufey` crate: `NativeTree::version()`,
`NativeTree::commit(&window,
&bytes) -> Result<(), NativeTreeError>`,
`NativeTree::on_event(|window_id, json|
...)`, `NativeTree::snapshot(&window)`,
and
`window_capabilities().native_tree() -> Option<NativeTreeCapability { version }>`.

### What a commit answers

| Answer             | Meaning                                                                      |
| ------------------ | ---------------------------------------------------------------------------- |
| `OK` (0)           | The batch decoded and is queued for the UI thread.                           |
| `UNSUPPORTED` (-1) | The backend has no native tree (in the `laufey` crate: the entry is `NULL`). |
| `MALFORMED` (-2)   | The batch does not decode. Nothing of it is applied.                         |
| `NO_WINDOW` (-3)   | No such window.                                                              |

A batch is checked twice. What needs no tree is checked on the calling thread,
and a fault there is `MALFORMED`: the header, every opcode, kind, prop id, value
tag, whether the value is one the prop takes, the ids that may not be 0, the
LISTEN bits, UTF-8, and that the bytes hold exactly `op_count` operations. What
needs the tree (an unknown node, a cycle, a `before` that is not a sibling) is
checked on the UI thread against the tree the batch meets there; a fault rejects
the whole batch, the tree stays at its last good state, and the event handler
hears

```json
{ "node": 0, "type": "batchError", "seq": 77, "error": "op 1: no node 999" }
```

with the batch's `seq`.

## Batch format, version 1 ("NTB1")

All integers are little-endian. A string is `u32 byte_len` followed by UTF-8.
Node id 0 means "none".

```
header:  magic "NTB1" (4 bytes) | version u16 = 1 | flags u16 = 0 | seq u32 | op_count u32
op:      opcode u8 | payload
```

| Opcode | Name       | Payload                                           | Notes                                                                                                                     |
| ------ | ---------- | ------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| 1      | CREATE     | id u32, kind u16                                  | The id is not in use. Ids are never reused in a window's lifetime (an id destroyed earlier in the batch is still in use). |
| 2      | INSERT     | parent u32, child u32, before u32                 | `before` 0 appends, else it is another child of the parent. Inserting an attached child moves it.                         |
| 3      | REMOVE     | parent u32, child u32                             | Detaches; the node stays alive.                                                                                           |
| 4      | DESTROY    | id u32                                            | Frees the node and its whole subtree. An attached node is detached first; the root is cleared when it was destroyed.      |
| 5      | SET_ROOT   | id u32                                            | The window's root: a node with no parent, or 0 for none.                                                                  |
| 6      | SET_STYLE  | id u32, n u16, n × (prop u16, value)              | Sets only the listed props; `unset` resets one to its default.                                                            |
| 7      | SET_PROP   | id u32, prop u16, value                           | A non-style prop.                                                                                                         |
| 8      | SET_TEXT   | id u32, string                                    | A RawText's content.                                                                                                      |
| 9      | LISTEN     | id u32, mask u32                                  | Replaces the node's event mask.                                                                                           |
| 10     | SET_IMAGE  | id u32, image_id u32                              | Points an Image node at an image (0: none). The image may arrive later.                                                   |
| 11     | IMAGE_DATA | image_id u32, mime string, bytes (u32 len + data) | The backend decodes off the UI thread and keeps the image until IMAGE_FREE. The same id again replaces it.                |
| 12     | IMAGE_FREE | image_id u32                                      |                                                                                                                           |

Only View, Text, ScrollView and Pressable take children. The root cannot be
inserted under a node, and a node cannot be inserted under itself or one of its
descendants.

**Values:** a tag byte, then the payload.

| Tag | Value   | Payload                            |
| --- | ------- | ---------------------------------- |
| 0   | unset   | none                               |
| 1   | f32     | 4 bytes (a length: logical pixels) |
| 2   | percent | f32 (`50.0` is 50%)                |
| 3   | auto    | none                               |
| 4   | enum    | u8, from the prop's table          |
| 5   | color   | u32 `0xRRGGBBAA`, straight alpha   |
| 6   | string  | string                             |
| 7   | bool    | u8, 0 or 1                         |
| 8   | f64     | 8 bytes                            |

Numbers must be finite. Which values a prop takes:

| Props                                                                                                                                                                                     | Values               |
| ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------- |
| `display`, `flexDirection`, `flexWrap`, `justifyContent`, `alignItems`, `alignSelf`, `alignContent`, `position`, `overflow`, `fontStyle`, `textAlign`, `textDecorationLine`, `resizeMode` | enum                 |
| `flex`, `flexGrow`, `flexShrink`, `aspectRatio`, `opacity`, `fontSize`, `fontWeight`, `lineHeight`, `letterSpacing`, `border*Width`, `border*Radius`, `eventCount`                        | f32 or f64           |
| `flexBasis`, `top` / `right` / `bottom` / `left`, `width`, `height`, `min*` / `max*`, `margin*`                                                                                           | f32, percent or auto |
| `gap`, `rowGap`, `columnGap`, `padding*`                                                                                                                                                  | f32 or percent       |
| `backgroundColor`, `border*Color`, `color`, `placeholderTextColor`                                                                                                                        | color                |
| `fontFamily`, `value`, `placeholder`, `accessibilityLabel`, `accessibilityRole`, `testID`                                                                                                 | string               |
| `editable`, `secureTextEntry`, `multiline`, `horizontal`, `scrollEnabled`, `disabled`                                                                                                     | bool                 |

Every prop also takes `unset`.

### The id tables

The kinds, style prop ids, non-style prop ids, enum values and LISTEN bits live
in one JSON file,
[`backends/gpui/protocol/props.json`](https://github.com/littledivy/laufey/blob/main/backends/gpui/protocol/props.json):
a byte-for-byte copy of thalora-web-browser `docs/b1-props.json` @ 7debb58. The
Rust table (`laufey_ntb1::ids`) is generated from it by the crate's `build.rs`,
and denext's TypeScript table from the same file. Ids are append-only within a
version; nothing is ever renumbered.

The crate `laufey-ntb1` (`backends/gpui/protocol`, no dependencies, no UI types)
holds the decoder, the reference encoder (`BatchBuilder`) and the retained tree
with its atomic `apply`; another backend's renderer can use it as is.

## Events

One JSON object per event: `{"node": id, "type": ...}` plus the fields below. A
node hears only the families its LISTEN mask names.

| LISTEN bit | Type                                        | Extra fields                                |
| ---------- | ------------------------------------------- | ------------------------------------------- |
| 1 press    | `pressIn`, `pressOut`, `press`, `longPress` | `x`, `y` (in the node)                      |
| 2 layout   | `layout`                                    | `x`, `y` (in the parent), `width`, `height` |
| 4 scroll   | `scroll`                                    | `x`, `y`, `contentWidth`, `contentHeight`   |
| 8 focus    | `focus`, `blur`                             |                                             |
| 16 text    | `changeText`                                | `text`, `eventCount`                        |
| 16 text    | `submitEditing`                             |                                             |
| 16 text    | `selectionChange`                           | `start`, `end` (UTF-16 units)               |
| 32 key     | `key`                                       | `key`, `modifiers` (`LAUFEY_MOD_*`)         |
| 64 image   | `imageLoad`, `imageError`                   | `image_id`                                  |
| (always)   | `batchError`                                | `seq`, `error`; `node` is 0                 |

- **Press** follows React Native: `pressIn` when the button goes down,
  `longPress` after 500 ms held, `pressOut` on release, then `press` when the
  release is inside the node and no long press fired. The innermost listening
  node takes the press. A node with `disabled` hears none.
- **Layout** is sent after a frame is laid out, only when the node's rectangle
  changed.
- **Scroll** is coalesced to one event per node per frame. `x` and `y` are how
  far the view has scrolled (zero or positive).
- **Text**: each edit by the user bumps the field's event count and reports it
  with `changeText`. A `value` the runtime sets is taken only when the node's
  `eventCount` prop is at least the field's count (or is not set), so a stale
  value never overwrites what the user typed since.

## The GPUI renderer

What `backends/gpui` does with each kind, and what version 1 leaves out:

| Kind            | GPUI                                                                                                     |
| --------------- | -------------------------------------------------------------------------------------------------------- |
| View, Pressable | `div`                                                                                                    |
| ScrollView      | `div` with a `ScrollHandle`; vertical, or horizontal with `horizontal`                                   |
| Text, RawText   | one `StyledText` per outermost Text: its RawText descendants, with a run per nested Text                 |
| Image           | `img` over a `RenderImage` decoded from IMAGE_DATA (PNG, JPEG, GIF, WebP, BMP, TIFF, ICO, SVG)           |
| TextInput       | a single-line field on GPUI's input handler: IME composition, the candidate window, clipboard, selection |

Styles are React Native's, with its defaults: every node is a flex container, a
column, and does not shrink; `flex: n` is grow `n`, shrink 1, basis 0. Text
props apply to Text and TextInput only and do not cascade from a View.

Not in version 1:

- `multiline` text inputs (the field stays one line), undo in a field, selection
  by double click.
- Per-side border colours: GPUI has one border colour, and the first side set
  (top, right, bottom, left) is used for all four.
- `letterSpacing`, `textAlign: justify`, `resizeMode: repeat` (drawn as
  `cover`), a nested Text's own `fontSize` / `fontFamily` (a run carries colour,
  weight, style, background and decoration), views inside text.
- `overflow: scroll` on anything but a ScrollView (it clips).
- Animated images play as GPUI plays them; there is no control over them.
- `accessibilityLabel`, `accessibilityRole` and `testID` are kept in the tree
  and not yet handed to the OS.
- Transforms and shadows (planned for version 2).
