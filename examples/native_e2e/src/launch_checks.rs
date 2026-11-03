//! The first window of a freshly launched app is on screen and its page
//! renders (LAUFEY_E2E_ONLY=launch-visibility).
//!
//! An app is usually started by something that is not itself frontmost: a
//! shell, an IDE's task runner, a CI agent. Before the fix, a WKWebView app
//! started that way on macOS could open its first window behind the active
//! app's windows; WebKit then reads the covered window as occluded, reports
//! `document.visibilityState` "hidden" and stops `requestAnimationFrame`
//! until the window is uncovered (React's scheduler, canvas and CSS-driven
//! animation all stall). scripts/native-e2e-run.sh --launch-visibility
//! starts a window from another process that covers the screen first
//! (scripts/launch-occluder.swift on macOS), so the launch has to bring its
//! window to the front for these checks to pass.
//!
//! The battery opens, in this order:
//!   1. a window created hidden and never shown: launching must not reveal it;
//!   2. the launch window, created hidden and shown once its page has loaded
//!      (the reveal Deno Desktop uses): its page load is reported, it is not
//!      visible before that show and is after it, and a frame is drawn within
//!      `FRAME_BUDGET`; the page's `outerWidth` / `outerHeight` are the
//!      window's frame, not 0;
//!   3. a second ordinary window, left unfocused when the launch window is
//!      focused again: visible, and frames are not throttled for lacking
//!      focus;
//!   4. a non-activating panel (a tray popover), never focused: visible and
//!      drawing frames.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

use laufey::{Value, Window, WindowOptions};

use crate::{check, na, wait_for};

/// How long a shown window may take to draw its first frame.
const FRAME_BUDGET: Duration = Duration::from_secs(2);

/// `expr` evaluated in `win`'s page, or `None` without an answer within 1 s.
async fn eval(win: &Window, expr: &str) -> Option<Value> {
  let (tx, rx) = tokio::sync::oneshot::channel();
  win.execute_js(
    expr,
    Some(move |r: Result<Value, Value>| {
      let _ = tx.send(r);
    }),
  );
  match tokio::time::timeout(Duration::from_secs(1), rx).await {
    Ok(Ok(Ok(v))) => Some(v),
    _ => None,
  }
}

fn as_f64(v: &Value) -> Option<f64> {
  match v {
    Value::Int(n) => Some(*n as f64),
    Value::Double(d) => Some(*d),
    _ => None,
  }
}

/// Waits until the page in `win` answers scripts from the app's own
/// document. The initial about:blank answers too, and a script run there
/// (a requestAnimationFrame asked for, a flag set) is lost with it when the
/// navigation commits: that race read as "WebKit runs no frames in a
/// panel" (e2e-testing.md §21).
async fn page_answers(win: &Window) -> bool {
  let deadline = Instant::now() + Duration::from_secs(20);
  while Instant::now() < deadline {
    if let Some(Value::String(s)) = eval(
      win,
      "location.protocol === 'laufey-e2e:' ? document.readyState : ''",
    )
    .await
    {
      if !s.is_empty() {
        return true;
      }
    }
    tokio::time::sleep(Duration::from_millis(100)).await;
  }
  false
}

/// The page's `document.visibilityState` once it reads `want`, or the last
/// value seen within `FRAME_BUDGET` (the engine learns about a show a moment
/// after the window system does).
async fn visibility(win: &Window, want: &str) -> String {
  let deadline = Instant::now() + FRAME_BUDGET;
  let mut last = String::from("(no answer)");
  loop {
    if let Some(Value::String(s)) = eval(win, "document.visibilityState").await
    {
      if s == want {
        return s;
      }
      last = s;
    }
    if Instant::now() >= deadline {
      return last;
    }
    tokio::time::sleep(Duration::from_millis(50)).await;
  }
}

/// How long the page took to run a `requestAnimationFrame` callback asked
/// for now, or `None` when none ran within `FRAME_BUDGET`. Measured here,
/// so script round trips count against the budget too.
async fn frame_delay(win: &Window, budget: Duration) -> Option<Duration> {
  let start = Instant::now();
  eval(
    win,
    "window.__e2eFrame = null; \
     requestAnimationFrame(() => { window.__e2eFrame = performance.now(); }); \
     true",
  )
  .await?;
  while start.elapsed() < budget {
    if let Some(v) = eval(win, "window.__e2eFrame").await {
      if as_f64(&v).is_some() {
        return Some(start.elapsed());
      }
    }
    tokio::time::sleep(Duration::from_millis(25)).await;
  }
  None
}

/// `[outerWidth, outerHeight, innerWidth, innerHeight]` of the page.
async fn outer_inner(win: &Window) -> Option<[f64; 4]> {
  match eval(
    win,
    "[window.outerWidth, window.outerHeight, window.innerWidth, \
     window.innerHeight]",
  )
  .await?
  {
    Value::List(l) if l.len() == 4 => Some([
      as_f64(&l[0])?,
      as_f64(&l[1])?,
      as_f64(&l[2])?,
      as_f64(&l[3])?,
    ]),
    _ => None,
  }
}

async fn has_focus(win: &Window) -> String {
  match eval(win, "document.hasFocus()").await {
    Some(Value::Bool(b)) => b.to_string(),
    _ => "(no answer)".into(),
  }
}

fn page(title: &str) -> String {
  format!("laufey-e2e://app/titled/{title}")
}

/// Whether `backend` keeps a window created hidden off screen until it is
/// shown. The WebView backends and CEF do; Winit windows show at creation.
fn hides_on_create(backend: &str) -> bool {
  backend == "webview" || backend == "cef"
}

pub async fn run() {
  let engine = laufey::scheme_handlers_supported();
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  let hides = hides_on_create(&backend);

  // 1. Created hidden, never shown.
  let hidden_title = "native-e2e-launch-hidden";
  let hidden = Window::new_with_options(
    320,
    240,
    WindowOptions {
      hidden: true,
      ..WindowOptions::default()
    },
  )
  .title(hidden_title)
  .load(&page(hidden_title));
  // The launch window opens once this page is live, not beside it. On a
  // virtual GPU (a macOS runner) two web processes starting together can
  // both open Metal's on-disk shader cache, and now and then the loser
  // blocks in flock() until the winner exits: docs/e2e-testing.md. Both
  // windows are still created before the launch reveal.
  #[cfg(target_os = "macos")]
  if engine {
    let _ = page_answers(&hidden).await;
  }

  // 2. The launch window, revealed once its page has loaded.
  let launch_title = "native-e2e-launch";
  let loaded = Arc::new(AtomicBool::new(false));
  let launch = Window::new_with_options(
    800,
    600,
    WindowOptions {
      hidden: true,
      ..WindowOptions::default()
    },
  )
  .title(launch_title)
  .on_page_load({
    let loaded = loaded.clone();
    move |_| loaded.store(true, Ordering::SeqCst)
  })
  .load(&page(launch_title));
  if engine {
    // Engine-less backends never load a page; reveal at once there. A
    // backend with an engine must report the load: it is what reveals a
    // window created hidden (CEF reported none, and showed every window at
    // creation instead until it honoured the hidden flag).
    check(
      "the launch window's page load is reported (on_page_load)",
      wait_for(|| loaded.load(Ordering::SeqCst), 200, 50).await,
    );
  }
  // Created hidden, its page loaded: still not on screen until show() (CEF
  // on macOS and Windows showed it at creation).
  if hides {
    check(
      "the launch window, created hidden, is not visible before show",
      !launch.get_visible(),
    );
  } else {
    na(&format!(
      "the launch window is hidden before show ({backend} has no \
       hidden-on-create)"
    ));
  }
  launch.show();
  check(
    "the launch window is visible after show",
    wait_for(|| launch.get_visible(), 100, 50).await,
  );

  if !engine {
    na("launch page visibility / frames / outer size (no web engine)");
  } else if !page_answers(&launch).await {
    check("the launch window's page answers scripts", false);
  } else {
    let vis = visibility(&launch, "visible").await;
    check(
      &format!("the launch page's visibilityState is visible (got {vis})"),
      vis == "visible",
    );
    match frame_delay(&launch, FRAME_BUDGET).await {
      Some(d) => check(
        &format!(
          "the launch page draws a frame within {} ms ({} ms)",
          FRAME_BUDGET.as_millis(),
          d.as_millis()
        ),
        true,
      ),
      None => check(
        &format!(
          "the launch page draws a frame within {} ms (none ran)",
          FRAME_BUDGET.as_millis()
        ),
        false,
      ),
    }
    eprintln!(
      "[e2e] INFO launch page document.hasFocus() = {}",
      has_focus(&launch).await
    );
    match outer_inner(&launch).await {
      Some([ow, oh, iw, ih]) => check(
        &format!(
          "the launch page's outerWidth/outerHeight are the window's \
           ({ow}x{oh}, inner {iw}x{ih})"
        ),
        ow > 0.0 && oh > 0.0 && ow >= iw && oh >= ih,
      ),
      None => check("the launch page reports its outer size", false),
    }
  }

  // 3. A second ordinary window, then the launch window focused again: the
  //    second one is on screen but not focused.
  let second_title = "native-e2e-launch-second";
  let second = Window::new(360, 240)
    .title(second_title)
    .load(&page(second_title));
  // Mostly to the right of the launch window, so that window coming back to
  // the front covers only part of it (a fully covered window is occluded,
  // and engines rightly stop drawing it).
  // Small enough for both to fit a 1024 wide runner screen, or macOS hides the
  // page of a window pushed off it.
  launch.set_size(400, 300);
  let _ = wait_for(|| launch.get_size().0 <= 404, 40, 50).await;
  let (lx, ly) = launch.get_position();
  let (lw, _) = launch.get_outer_size();
  second.set_position(lx + lw - 100, ly + 40);
  // Only the first window is ordered in front of other apps. Where the system
  // declines this app's activation (a macOS 15 runner), a later window stays
  // behind the occluder, and a covered window rightly reads as hidden. These
  // checks are about lacking focus, so keep it uncovered.
  #[cfg(target_os = "macos")]
  second.set_level(laufey::LAUFEY_WINDOW_LEVEL_FLOATING);
  if engine && page_answers(&second).await {
    launch.focus();
    tokio::time::sleep(Duration::from_millis(300)).await;
    eprintln!(
      "[e2e] INFO unfocused window document.hasFocus() = {}",
      has_focus(&second).await
    );
    let vis = visibility(&second, "visible").await;
    check(
      &format!("an unfocused window's page is visible (got {vis})"),
      vis == "visible",
    );
    let d = frame_delay(&second, FRAME_BUDGET).await;
    check(
      &format!(
        "an unfocused window's page draws a frame within {} ms ({})",
        FRAME_BUDGET.as_millis(),
        d.map_or("none ran".into(), |d| format!("{} ms", d.as_millis()))
      ),
      d.is_some(),
    );
  } else if engine {
    check("the unfocused window's page answers scripts", false);
  }
  #[cfg(target_os = "macos")]
  second.set_level(laufey::LAUFEY_WINDOW_LEVEL_NORMAL);

  // 4. A non-activating panel (tray / menu-bar popover), never focused.
  let panel_title = "native-e2e-launch-panel";
  let panel = Window::new_with_options(
    300,
    200,
    WindowOptions {
      no_activate: true,
      ..WindowOptions::default()
    },
  )
  .title(panel_title)
  .load(&page(panel_title));
  panel.set_position(420, 80);
  if engine && page_answers(&panel).await {
    let vis = visibility(&panel, "visible").await;
    check(
      &format!("a non-activating panel's page is visible (got {vis})"),
      vis == "visible",
    );
    let d = frame_delay(&panel, FRAME_BUDGET).await;
    let name = format!(
      "a non-activating panel's page draws a frame within {} ms ({})",
      FRAME_BUDGET.as_millis(),
      d.map_or("none ran".into(), |d| format!("{} ms", d.as_millis()))
    );
    check(&name, d.is_some());
  } else if engine {
    check("the panel's page answers scripts", false);
  }

  // Back to 1, after both reveals: still hidden. The WebView backends and
  // CEF implement hidden-on-create; Winit windows show at creation.
  if !hides {
    na(&format!(
      "a hidden window stays hidden at launch ({backend} has no \
       hidden-on-create)"
    ));
  } else {
    check(
      "a hidden window stays hidden through the launch reveal",
      !hidden.get_visible(),
    );
  }

  // The windows stay open: finish() ends the process, and closing the last
  // one first would race the backend's quit-on-last-window.
  let _ = (&hidden, &launch, &second, &panel);
}
