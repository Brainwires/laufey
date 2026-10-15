# The share sheet

`share` (API 49) shows the OS share sheet over a window for some text, a URL and
files, and reports how it ended.

```rust
laufey::share(window.id(), &laufey::ShareRequest {
  title: Some("Report".into()),
  text: Some("Here is the report".into()),
  url: Some("https://example.com/r/1".into()),
  files: vec!["/Users/me/report.pdf".into()],
  point: None,
}, |outcome| println!("{outcome:?}"));
```

| OS      | Sheet                                       | Outcome                                                                                                |
| ------- | ------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| macOS   | `NSSharingServicePicker` over the window    | `Completed` (the service's name) or `Cancelled`                                                        |
| Windows | `DataTransferManager` for the window's HWND | `Completed` / `Cancelled` where Windows reports them (`ShareCompleted`, `ShareCanceled`), else `Shown` |
| Linux   | none                                        | `NotSupported`                                                                                         |

The request needs at least one of `text`, `url` or `files`; `url` must be an
absolute URL and `files` absolute paths. On macOS a file that doesn't exist is
left out (and a request left with nothing fails), and `point` anchors the picker
at that point of the window (DIPs; by default its middle). On Windows files go
as their paths in the shared text: a storage item needs an asynchronous lookup
the share request can't wait for. Windows requires a title; laufey uses the
text's start, or "Share", when there is none.

The callback fires exactly once, on the UI thread, or synchronously for a
refusal (a bad request, an unknown window).
