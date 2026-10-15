# User activities: Handoff and Spotlight

`set_user_activity` (API 49) makes an activity the app is engaged in its current
`NSUserActivity` on macOS: the user's other Apple devices offer it (Handoff),
and a `searchable` one is indexed by Spotlight. `NULL` ends it.

```rust
laufey::set_user_activity(Some(&laufey::UserActivity {
  activity_type: "com.example.app.document".into(),
  title: "Quarterly report".into(),
  url: Some("https://example.com/d/42".into()),
  user_info: [("id".to_string(), "42".to_string())].into(),
  keywords: vec!["report".into()],
  handoff: true,
  searchable: true,
}));
laufey::on_user_activity(|json| println!("continued: {json}"));
```

- `activity_type` must be reverse-DNS and, for another device or a Spotlight
  result to continue it in this app, listed in the bundle's
  `NSUserActivityTypes` (Info.plist).
- `url` (http or https) is the `webpageURL` a device without the app opens in a
  browser.
- `user_info` holds string values; all its keys are required on continuation.
- A continued activity reaches the `on_user_activity` handler as
  `{"type", "title", "url", "userInfo"}`, through the app delegate's
  `application:continueUserActivity:restorationHandler:`. One continued before a
  handler exists (the one that launched the app) is held (at most 16) and
  delivered when it registers.

Windows and Linux have no equivalent: `set_user_activity` returns `false`.
