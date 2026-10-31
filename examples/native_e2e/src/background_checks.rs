// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Background throttling (API 51), part of the window-api checks
//! (`--window-api`):
//!
//! - background_throttling reads back the policy a window was created with
//!   on the macOS WebView backend, and none everywhere else;
//! - macOS WebView: the page of a window created hidden keeps running its
//!   timers long after it loaded: throttled by default (WKWebView's own
//!   default, Suspend, froze it about a second after the load), at full rate
//!   with BackgroundThrottling::None. A Suspend window's count is reported,
//!   not checked: when WebKit freezes a page is its own business.

use std::time::Duration;

use laufey::{BackgroundThrottling, Window, WindowOptions};

use crate::{check, na, page_number};

fn hidden_window(policy: BackgroundThrottling) -> Window {
  let title = format!("native-e2e-background-{policy:?}");
  Window::new_with_options(
    320,
    240,
    WindowOptions {
      hidden: true,
      background_throttling: policy,
      ..Default::default()
    },
  )
  .title(&title)
  .load(&format!("laufey-e2e://app/titled/{title}"))
}

pub async fn run() {
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  let policies = [
    BackgroundThrottling::Throttle,
    BackgroundThrottling::None,
    BackgroundThrottling::Suspend,
  ];
  let windows: Vec<Window> =
    policies.iter().map(|p| hidden_window(*p)).collect();

  let chooses = backend == "webview" && cfg!(target_os = "macos");
  if !chooses {
    check(
      &format!(
        "{backend} on {}: no background policy is reported",
        std::env::consts::OS
      ),
      windows.iter().all(|w| w.background_throttling().is_none()),
    );
    windows.iter().for_each(Window::close);
    return;
  }

  for (w, policy) in windows.iter().zip(policies) {
    check(
      &format!("a window created with {policy:?} reads it back"),
      w.background_throttling() == Some(policy),
    );
  }

  // The pages loaded (they answer), then tick every 100 ms, stamped from
  // now. The windows were never on screen.
  for w in &windows {
    if page_number(w, "1").await.is_none() {
      na("a hidden window's page never answered (no page to run timers)");
      windows.iter().for_each(Window::close);
      return;
    }
  }
  for w in &windows {
    page_number(
      w,
      "window.__ticks = []; window.__start = performance.now(); \
       setInterval(() => __ticks.push(performance.now() - __start), 100); 1",
    )
    .await;
  }
  tokio::time::sleep(Duration::from_secs(10)).await;
  // Asking runs a page again even where it was frozen, so count only the
  // ticks between 4 s and 9 s, before the question: a frozen page has none
  // there, a throttled one about one a second, an unthrottled one ~50.
  let mut counts = Vec::new();
  for w in &windows {
    counts.push(
      page_number(w, "__ticks.filter((t) => t > 4000 && t < 9000).length")
        .await
        .unwrap_or(0.0),
    );
  }
  eprintln!(
    "[e2e] hidden pages, timer ticks between 4 s and 9 s: throttle {}, \
     none {}, suspend {}",
    counts[0], counts[1], counts[2]
  );
  check(
    "a hidden window's page keeps running its timers by default (throttled)",
    counts[0] >= 2.0,
  );
  check(
    "with BackgroundThrottling::None its timers keep their rate",
    counts[1] >= 30.0,
  );
  windows.iter().for_each(Window::close);
}
