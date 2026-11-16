// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Background throttling (API 51), part of the window-api checks
//! (`--window-api`, or alone with LAUFEY_E2E_ONLY=background):
//!
//! - background_throttling reads back the policy a window was created with
//!   on the macOS WebView backend, and none everywhere else;
//! - macOS WebView: the page of a window created hidden keeps running its
//!   timers long after it loaded: throttled by default (WKWebView's own
//!   default, Suspend, froze it about a second after the load), at full rate
//!   with BackgroundThrottling::None. A Suspend window's count is reported,
//!   not checked: when WebKit freezes a page is its own business.
//!
//! - macOS WebView: a None window hidden after it was shown (hide()) keeps
//!   its page's rate too: it stays ordered in but invisible (transparent,
//!   clicks passing through, not key, not reported visible), while one
//!   created with MacosHiddenMode::OrderOut is really ordered out.
//!
//! Each policy is measured alone (its window the only hidden one) and then
//! all three at once (the windows share WebKit's process pool; the policy is
//! per page). LAUFEY_E2E_BACKGROUND_REPEAT=N runs those rounds N times;
//! LAUFEY_E2E_BACKGROUND_IDLE_S=S leaves the pages alone for S seconds
//! (default and minimum 10) before asking them.

use std::time::Duration;

use laufey::{BackgroundThrottling, Value, Window, WindowOptions};

use crate::{check, na, page_number};

const POLICIES: [BackgroundThrottling; 3] = [
  BackgroundThrottling::Throttle,
  BackgroundThrottling::None,
  BackgroundThrottling::Suspend,
];

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

/// A string the page computes from `expr`, or `None` if it doesn't answer
/// one within a second.
async fn page_string(win: &Window, expr: &str) -> Option<String> {
  let (tx, rx) = tokio::sync::oneshot::channel();
  win.execute_js(
    expr,
    Some(move |r: Result<Value, Value>| {
      let _ = tx.send(r);
    }),
  );
  match tokio::time::timeout(Duration::from_secs(1), rx).await {
    Ok(Ok(Ok(Value::String(s)))) => Some(s),
    _ => None,
  }
}

/// Whether `w` shows its own page within 10 s. A new web view starts on an
/// empty document that answers scripts too, until the load commits and
/// replaces it, taking anything a script set up there with it: an interval
/// started that early never ticks in the real page (which read as a frozen
/// page in 2 of 7 runs, always the window asked first).
async fn page_loaded(w: &Window) -> bool {
  for _ in 0..100 {
    let loaded = page_number(
      w,
      "location.protocol === 'laufey-e2e:' && \
       document.readyState === 'complete' ? 1 : 0",
    )
    .await;
    match loaded {
      Some(1.0) => return true,
      None => return false,
      _ => tokio::time::sleep(Duration::from_millis(100)).await,
    }
  }
  false
}

/// How one hidden page ran.
enum Ran {
  /// Its 100 ms interval's ticks inside the counted span.
  Ticks(f64),
  /// The interval was gone when asked: the page was replaced.
  Replaced,
}

/// One round: hidden windows for `policies`, open together, their pages'
/// 100 ms interval counted from 4 s after it started to 1 s before the
/// question (`idle` s after the start). `None` for a page that never loaded.
async fn round(
  label: &str,
  policies: &[BackgroundThrottling],
  idle: u64,
) -> Option<Vec<Ran>> {
  // Opened one at a time, each once the one before has its page. On a
  // virtual GPU (a macOS runner) two web processes starting together can both
  // open Metal's on-disk shader cache, and now and then the loser blocks in
  // flock() until the winner exits: docs/e2e-testing.md. The timers are
  // started only once all of them are open.
  let mut windows: Vec<Window> = Vec::new();
  for p in policies {
    if let Some(prev) = windows.last() {
      let _ = page_loaded(prev).await;
    }
    windows.push(hidden_window(*p));
  }
  for (w, policy) in windows.iter().zip(policies) {
    check(
      &format!("{label}: a window created with {policy:?} reads it back"),
      w.background_throttling() == Some(*policy),
    );
  }
  // The pages loaded, then tick every 100 ms, stamped from now. The windows
  // were never on screen.
  for w in &windows {
    if !page_loaded(w).await {
      windows.iter().for_each(Window::close);
      return None;
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
  tokio::time::sleep(Duration::from_secs(idle)).await;
  // Asking runs a page again even where it was frozen, so count only the
  // ticks before the question: a frozen page has none there, a throttled
  // one about one a second, an unthrottled one ~10.
  let end = (idle - 1) * 1000;
  let mut ran = Vec::new();
  for (w, policy) in windows.iter().zip(policies) {
    let n = page_number(
      w,
      &format!(
        "typeof __ticks === 'undefined' ? -1 : \
         __ticks.filter((t) => t > 4000 && t < {end}).length"
      ),
    )
    .await
    .unwrap_or(0.0);
    // Where it ran: the last tick before the question and the longest gap,
    // so a freeze says when it began.
    let shape = page_string(
      w,
      "typeof __ticks === 'undefined' ? 'page replaced' : (() => { \
       let g = [0, 0], p = 0; for (const t of __ticks) { \
       if (t - p > g[1]) g = [p, t - p]; p = t; } \
       return `ticks ${__ticks.length}, last ${Math.round(p)} ms, \
       longest gap ${Math.round(g[1])} ms from ${Math.round(g[0])} ms`; })()",
    )
    .await
    .unwrap_or_else(|| "no answer".into());
    eprintln!(
      "[e2e] {label}: {policy:?} {n} ticks in 4-{} s ({shape})",
      idle - 1
    );
    ran.push(if n < 0.0 {
      Ran::Replaced
    } else {
      Ran::Ticks(n)
    });
  }
  windows.iter().for_each(Window::close);
  Some(ran)
}

/// Ticks of a 100 ms interval in the page of `w` from 4 s after it started
/// to 1 s before the question, `idle` s later.
#[cfg(target_os = "macos")]
async fn ticks_after(w: &Window, idle: u64) -> f64 {
  page_number(
    w,
    "window.__ticks = []; window.__start = performance.now(); \
     setInterval(() => __ticks.push(performance.now() - __start), 100); 1",
  )
  .await;
  tokio::time::sleep(Duration::from_secs(idle)).await;
  let end = (idle - 1) * 1000;
  page_number(
    w,
    &format!(
      "typeof __ticks === 'undefined' ? -1 : \
       __ticks.filter((t) => t > 4000 && t < {end}).length"
    ),
  )
  .await
  .unwrap_or(0.0)
}

/// macOS: hide() on a shown None window keeps it ordered in, invisible, and
/// its page at full rate; MacosHiddenMode::OrderOut really orders it out.
#[cfg(target_os = "macos")]
async fn hidden_after_show(idle: u64) {
  use crate::overlay_checks::mac;
  use laufey::MacosHiddenMode;
  let span = (idle - 5) as f64;
  for mode in [MacosHiddenMode::Invisible, MacosHiddenMode::OrderOut] {
    let title = format!("native-e2e-background-shown-{mode:?}");
    let w = Window::new_with_options(
      320,
      240,
      WindowOptions {
        background_throttling: BackgroundThrottling::None,
        macos_hidden_mode: mode,
        ..Default::default()
      },
    )
    .title(&title)
    .load(&format!("laufey-e2e://app/titled/{title}"));
    if !page_loaded(&w).await {
      na(&format!("{mode:?}: a shown window's page never loaded"));
      w.close();
      continue;
    }
    w.show();
    w.focus();
    tokio::time::sleep(Duration::from_millis(500)).await;
    w.hide();
    tokio::time::sleep(Duration::from_millis(500)).await;
    let t = title.clone();
    let native = laufey::spawn_on_ui_thread(move || {
      (
        mac::window_flag(&t, c"isVisible"),
        mac::window_flag(&t, c"isKeyWindow"),
        mac::window_flag(&t, c"ignoresMouseEvents"),
      )
    })
    .await
    .unwrap_or((None, None, None));
    let n = ticks_after(&w, idle).await;
    eprintln!(
      "[e2e] None, shown then hidden ({mode:?}): {n} ticks in 4-{} s; \
       visible {}, native (ordered in, key, ignores mouse) {native:?}",
      idle - 1,
      w.get_visible()
    );
    check(
      &format!("a hidden None window ({mode:?}) is not reported visible"),
      !w.get_visible(),
    );
    match mode {
      MacosHiddenMode::Invisible => {
        check(
          "hide() keeps a None window ordered in but invisible: clicks pass \
           through and it is not key",
          native == (Some(true), Some(false), Some(true)),
        );
        check(
          "a None window hidden after it was shown keeps its timers' rate",
          n >= span * 6.0,
        );
      }
      MacosHiddenMode::OrderOut => check(
        "MacosHiddenMode::OrderOut really orders a hidden None window out",
        native.0 == Some(false),
      ),
    }
    // Shown again: on screen, taking clicks.
    w.show();
    tokio::time::sleep(Duration::from_millis(500)).await;
    let t = title.clone();
    let shown = laufey::spawn_on_ui_thread(move || {
      (
        mac::window_flag(&t, c"isVisible"),
        mac::window_flag(&t, c"ignoresMouseEvents"),
      )
    })
    .await
    .unwrap_or((None, None));
    check(
      &format!(
        "a hidden None window ({mode:?}) shown again is visible and takes \
         clicks ({shown:?}, visible {})",
        w.get_visible()
      ),
      w.get_visible() && shown == (Some(true), Some(false)),
    );
    w.close();
  }
}

fn env_number(name: &str, default: u64) -> u64 {
  std::env::var(name)
    .ok()
    .and_then(|v| v.parse::<u64>().ok())
    .unwrap_or(default)
}

pub async fn run() {
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  let chooses = backend == "webview" && cfg!(target_os = "macos");
  if !chooses {
    let windows: Vec<Window> =
      POLICIES.iter().map(|p| hidden_window(*p)).collect();
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

  let repeat = env_number("LAUFEY_E2E_BACKGROUND_REPEAT", 1).max(1);
  let idle = env_number("LAUFEY_E2E_BACKGROUND_IDLE_S", 10).max(10);
  // Seconds counted; throttled timers run about once a second, unthrottled
  // ones every 100 ms (at 10 s idle: >= 2 and >= 30 ticks).
  let span = (idle - 5) as f64;
  // (pages measured, pages that kept their expected rate)
  let mut throttle = (0u32, 0u32);
  let mut unthrottled = (0u32, 0u32);
  let mut no_page = false;
  let mut replaced = false;
  for i in 1..=repeat {
    let mut rounds: Vec<(String, Vec<BackgroundThrottling>)> = POLICIES
      .iter()
      .map(|p| (format!("round {i}, {p:?} alone"), vec![*p]))
      .collect();
    rounds.push((format!("round {i}, all three"), POLICIES.to_vec()));
    for (label, policies) in rounds {
      let Some(ran) = round(&label, &policies, idle).await else {
        no_page = true;
        continue;
      };
      for (policy, ran) in policies.iter().zip(ran) {
        let n = match ran {
          Ran::Ticks(n) => n,
          Ran::Replaced => {
            replaced = true;
            continue;
          }
        };
        match policy {
          BackgroundThrottling::Throttle => {
            throttle.0 += 1;
            throttle.1 += u32::from(n >= span * 0.4);
          }
          BackgroundThrottling::None => {
            unthrottled.0 += 1;
            unthrottled.1 += u32::from(n >= span * 6.0);
          }
          _ => {}
        }
      }
    }
  }
  if no_page {
    na("a hidden window's page never loaded (no page to run timers)");
  }
  check(
    "the hidden pages are still the pages their timers were started in",
    !replaced,
  );
  eprintln!(
    "[e2e] hidden pages over {repeat} round(s), {idle} s idle: throttled \
     kept running in {}/{}, unthrottled kept its rate in {}/{}",
    throttle.1, throttle.0, unthrottled.1, unthrottled.0
  );
  check(
    "a hidden window's page keeps running its timers by default (throttled), \
     alone and beside other hidden windows",
    throttle.0 > 0 && throttle.1 == throttle.0,
  );
  check(
    "with BackgroundThrottling::None its timers keep their rate, alone and \
     beside other hidden windows",
    unthrottled.0 > 0 && unthrottled.1 == unthrottled.0,
  );
  #[cfg(target_os = "macos")]
  hidden_after_show(idle).await;
}
