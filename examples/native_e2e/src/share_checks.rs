// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The share sheet (API 49; docs/share.md). Run with `LAUFEY_E2E_ONLY=share`
//! by `native-e2e-run.sh --share`. The sheet itself waits for a person, so
//! the battery checks what answers without one: a request with nothing to
//! share, an unknown window, a file that doesn't exist (macOS), and Linux's
//! NOT_SUPPORTED.

use std::time::Duration;

use laufey::{ShareOutcome, ShareRequest, UserActivity, Window};

use super::{check, na};

async fn share(window_id: u32, request: ShareRequest) -> Option<ShareOutcome> {
  let (tx, rx) = tokio::sync::oneshot::channel();
  laufey::share(window_id, &request, move |o| {
    let _ = tx.send(o);
  });
  tokio::time::timeout(Duration::from_secs(10), rx)
    .await
    .ok()
    .and_then(|r| r.ok())
}

pub async fn run() {
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  if backend == "winit" {
    na("share (Winit has no share sheet)");
    return;
  }
  user_activity_checks();
  let win = Window::new(320, 240).title("native-e2e-share");
  win.show();
  tokio::time::sleep(Duration::from_millis(500)).await;

  let empty = share(win.id(), ShareRequest::default()).await;
  check(
    &format!("share: a request with nothing to share fails ({empty:?})"),
    matches!(empty, Some(ShareOutcome::Failed(Some(_)))),
  );
  let text = ShareRequest {
    text: Some("hello".into()),
    ..Default::default()
  };
  if cfg!(target_os = "linux") {
    let got = share(win.id(), text).await;
    check(
      &format!("share: Linux has no share sheet ({got:?})"),
      matches!(got, Some(ShareOutcome::NotSupported(_))),
    );
    return;
  }
  let unknown = share(999_999, text).await;
  check(
    &format!("share: an unknown window fails ({unknown:?})"),
    matches!(unknown, Some(ShareOutcome::Failed(Some(_)))),
  );
  if cfg!(target_os = "macos") {
    let missing = share(
      win.id(),
      ShareRequest {
        files: vec!["/nonexistent/laufey-share-e2e.txt".into()],
        ..Default::default()
      },
    )
    .await;
    check(
      &format!("share: a missing file leaves nothing to share ({missing:?})"),
      matches!(missing, Some(ShareOutcome::Failed(Some(_)))),
    );
  }
  let _ = &win;
}

/// User activities (API 49): set and cleared on macOS, refused elsewhere;
/// an invalid activity is refused everywhere.
fn user_activity_checks() {
  let activity = UserActivity {
    activity_type: "dev.laufey.e2e.view".into(),
    title: "laufey e2e".into(),
    url: Some("https://example.com/e2e".into()),
    handoff: true,
    searchable: false,
    ..Default::default()
  };
  let set = laufey::set_user_activity(Some(&activity));
  check(
    &format!("user activity: set where there are activities (macOS) ({set})"),
    set == cfg!(target_os = "macos"),
  );
  let invalid = UserActivity {
    activity_type: "nodots".into(),
    ..Default::default()
  };
  check(
    "user activity: an activity type that isn't reverse-DNS is refused",
    !laufey::set_user_activity(Some(&invalid)),
  );
  let cleared = laufey::set_user_activity(None);
  check(
    &format!("user activity: cleared on macOS ({cleared})"),
    cleared == cfg!(target_os = "macos"),
  );
  let handler = laufey::on_user_activity(|_| {});
  check("user activity: the continuation handler registers", handler);
}
