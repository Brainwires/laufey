# Message dialogs

laufey shows the operating system's own message box, with the app's button
labels: `NSAlert` on macOS, `TaskDialogIndirect` on Windows and
`GtkMessageDialog` on Linux. The same native dialog is used by every backend of
an OS, CEF included. Unlike the [page dialogs](dialogs.md) (`alert()`,
`confirm()`), it never blocks the caller: the answer comes back through a
callback, or a future in Rust.

```rust
let dialog = laufey::show_message_dialog(
  window.id(), // 0: app-modal, also with no window open
  &laufey::MessageDialogOptions {
    kind: laufey::MessageDialogKind::Warning,
    message: "Save the changes before quitting?".into(),
    detail: Some("Your changes are lost if you don't save them.".into()),
    buttons: vec!["Save".into(), "Don't Save".into(), "Cancel".into()],
    default_index: Some(0),
    cancel_index: Some(2),
    ..Default::default()
  },
);
match dialog.outcome.await {
  laufey::MessageDialogOutcome::Answered(0) => save(),
  laufey::MessageDialogOutcome::Answered(1) => quit(),
  _ => {} // Cancel, dismissed, or it could not be shown
}
```

## API

C ABI (API ≥ 54):

```c
uint32_t (*show_message_dialog)(void* backend_data, uint32_t window_id,
                                const laufey_message_dialog_options_t* options,
                                laufey_message_dialog_result_fn callback,
                                void* user_data);

typedef struct laufey_message_dialog_options {
  int kind;  // LAUFEY_MESSAGE_DIALOG_INFO / _WARNING / _ERROR
  const char* title;
  const char* message;
  const char* detail;
  const char* const* buttons;
  size_t button_count;
  int32_t default_index;  // -1: the first button
  int32_t cancel_index;   // -1: none
} laufey_message_dialog_options_t;

typedef void (*laufey_message_dialog_result_fn)(void* user_data,
                                                uint32_t dialog_id, int status,
                                                int32_t response);
```

- Any thread. The backend shows the dialog on its UI thread (on Windows on its
  I/O thread, as the file dialogs, so the dialog's modal loop never stalls the
  engine) and returns its id at once.
- `callback` fires exactly once: `LAUFEY_MESSAGE_DIALOG_ANSWERED` with the
  pressed button's index, `LAUFEY_MESSAGE_DIALOG_CANCELLED` (dismissed without a
  button) or `LAUFEY_MESSAGE_DIALOG_FAILED` (bad options, no display). Bad
  options are answered synchronously, before `show_message_dialog` returns 0.
- `message` is required. With no buttons the dialog has one "OK". At most
  `LAUFEY_MESSAGE_DIALOG_MAX_BUTTONS` (16) labels; an index out of range is the
  default (the first button; no cancel button).
- Several dialogs may be open at once; each has its own id.
- `LAUFEY_WINDOW_CAP_MESSAGE_DIALOGS` in `window_capabilities` says the backend
  has them: every CEF and WebView backend on macOS, Windows and Linux. The Winit
  backends have none (`show_message_dialog` is NULL; Rust answers `Failed`).

## Modality and no window

A `window_id` of an open, visible window makes the dialog modal to it: a sheet
on macOS (unless the window already has one), an owned dialog on Windows, a
transient dialog on Linux (WebKitGTK; under CEF the GTK dialog can't be
transient for Chromium's window and is app-modal). With 0, or a window that is
gone, the dialog is app-modal and needs no window of the app's: a tray app or a
quit guard can ask with every window closed. Such a dialog brings the app to the
front (macOS activates the app; Windows sets the dialog in the foreground; Linux
keeps it above, centred).

On macOS an app-modal alert runs `runModal` from the main run loop, not from a
main-queue block, so the backend's main-queue work keeps running while it is up.

## Buttons, Return and Escape

|                          | macOS                  | Windows         | Linux           |
| ------------------------ | ---------------------- | --------------- | --------------- |
| Layout                   | first button rightmost | left to right   | left to right   |
| Return                   | `default_index`        | `default_index` | `default_index` |
| Escape, cancel button    | presses it             | presses it      | presses it      |
| Escape, no cancel button | nothing                | `CANCELLED`     | `CANCELLED`     |
| Close button             | none                   | as Escape       | `CANCELLED`     |

On Windows and Linux, Escape and the close button press the cancel button when
there is one, so the result is `ANSWERED` with `cancel_index`, as on macOS.
Quitting the app ends an open Linux dialog as `CANCELLED`.

Windows: `TaskDialogIndirect` exists only in Common Controls 6. A host without a
Common Controls 6 manifest (a runtime's own executable) gets it for the call
through an activation context. Should it still be missing, a one-button dialog
falls back to `MessageBoxW` and any other fails.

## Tests

`backend-common/tests/message_dialog_test.cc` covers the options (the copy, the
defaults, the refusals), the ids and the exactly-once result without an OS; the
`capi` unit tests cover the Rust wrapper against a fake backend.
