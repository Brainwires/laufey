// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The navigation guard (API 48; docs/window-management.md). Run with
//! `LAUFEY_E2E_ONLY=navigation` by `native-e2e-run.sh --navigation`.
//!
//! A window navigated to a loopback server (`http://127.0.0.1:<port>`, its
//! own origin) with the open-external capture on, so nothing reaches a real
//! browser:
//!
//! - a same-origin navigation loads;
//! - a script navigation to another origin (`http://localhost:<port>`) does
//!   not, and its URL is what would have opened in the browser;
//! - so is a meta refresh there, and a same-origin request the server
//!   redirects to the other origin;
//! - a `window.open()` opens no web view (and goes to the browser only
//!   with a user gesture);
//! - an iframe on the other origin still loads (sub-frames are the CSP's);
//! - a navigation the embedder's will-navigate handler refuses (API 56),
//!   answered at once or a moment later from another thread, from an
//!   http(s) page and from a custom-scheme one, leaves the page as it was:
//!   its bridge calls are answered and its fetches load while the
//!   navigation is pending and after (as the page's origin), and its timers
//!   run; and so it does for 30 refused navigations in a row (WebKitGTK
//!   once decided at the response, after the load had started and the
//!   page's own loads had begun to fail);
//! - once the embedder allows the other origin, a navigation there loads.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{NavigationAnswer, Value, Window};

use super::{check, na, wait_for};

const PAGE: &str = "<!doctype html><html><head><meta charset=\"utf-8\">\
<title>navigation</title></head><body>navigation</body></html>";

type Hits = Arc<Mutex<Vec<String>>>;

/// The request target of an HTTP/1.1 request head.
fn path_of(head: &str) -> String {
  head
    .lines()
    .next()
    .and_then(|l| l.split_whitespace().nth(1))
    .unwrap_or("")
    .to_string()
}

fn serve(mut stream: TcpStream, port: u16, hits: &Hits) {
  let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
  let mut buf = Vec::new();
  let mut chunk = [0u8; 2048];
  while !buf.windows(4).any(|w| w == b"\r\n\r\n") {
    match stream.read(&mut chunk) {
      Ok(0) | Err(_) => return,
      Ok(n) => buf.extend_from_slice(&chunk[..n]),
    }
    if buf.len() > 64 * 1024 {
      return;
    }
  }
  let path = path_of(&String::from_utf8_lossy(&buf));
  hits.lock().unwrap().push(path.clone());
  let response = match path.as_str() {
    "/redirect" => format!(
      "HTTP/1.1 302 Found\r\nlocation: http://localhost:{port}/redirected\r\n\
       content-length: 0\r\nconnection: close\r\n\r\n"
    ),
    "/live" => format!(
      "HTTP/1.1 200 OK\r\ncontent-type: text/html; charset=utf-8\r\n\
       cache-control: no-store\r\ncontent-length: {}\r\n\
       connection: close\r\n\r\n{LIVE_PAGE}",
      LIVE_PAGE.len()
    ),
    "/favicon.ico" => {
      "HTTP/1.1 404 Not Found\r\ncontent-length: 0\r\nconnection: close\r\n\r\n"
        .to_string()
    }
    _ => format!(
      "HTTP/1.1 200 OK\r\ncontent-type: text/html; charset=utf-8\r\n\
       cache-control: no-store\r\ncontent-length: {}\r\n\
       connection: close\r\n\r\n{PAGE}",
      PAGE.len()
    ),
  };
  let _ = stream.write_all(response.as_bytes());
}

/// A loopback HTTP server on its own thread: its port and the paths asked.
fn start_server() -> Option<(u16, Hits)> {
  let listener = TcpListener::bind("127.0.0.1:0").ok()?;
  let port = listener.local_addr().ok()?.port();
  let hits: Hits = Arc::new(Mutex::new(Vec::new()));
  let server_hits = hits.clone();
  std::thread::spawn(move || {
    for stream in listener.incoming().flatten() {
      let hits = server_hits.clone();
      std::thread::spawn(move || serve(stream, port, &hits));
    }
  });
  Some((port, hits))
}

async fn eval_string(win: &Window, script: &str) -> Option<String> {
  for _ in 0..30 {
    let (tx, rx) = tokio::sync::oneshot::channel();
    win.execute_js(
      script,
      Some(move |r: Result<Value, Value>| {
        let _ = tx.send(r);
      }),
    );
    match tokio::time::timeout(Duration::from_secs(1), rx).await {
      Ok(Ok(Ok(v))) => return v.as_string().map(str::to_string),
      _ => tokio::time::sleep(Duration::from_millis(200)).await,
    }
  }
  None
}

/// Where the window's document is (`origin + pathname`), once a script
/// answers.
async fn location(win: &Window) -> Option<String> {
  eval_string(win, "String(location.origin) + String(location.pathname)").await
}

/// Polls [`location`] until it is `want` (or about `tries` × 200 ms pass).
async fn wait_location(win: &Window, want: &str, tries: u32) -> Option<String> {
  let mut last = None;
  for _ in 0..tries {
    last = location(win).await;
    if last.as_deref() == Some(want) {
      break;
    }
    tokio::time::sleep(Duration::from_millis(200)).await;
  }
  last
}

/// Waits until the capture holds `url` (draining it into `seen`).
async fn wait_captured(seen: &Mutex<Vec<String>>, url: &str) -> bool {
  for _ in 0..50 {
    if let Some(more) = laufey::test_take_external_opens() {
      seen.lock().unwrap().extend(more);
    }
    if seen.lock().unwrap().iter().any(|u| u == url) {
      return true;
    }
    tokio::time::sleep(Duration::from_millis(200)).await;
  }
  false
}

pub async fn run() {
  if !laufey::scheme_handlers_supported() {
    na("navigation (backend has no web engine)");
    return;
  }
  if !laufey::test_capture_external_opens(true) {
    check(
      "navigation: the backend has the navigation guard (API 48)",
      false,
    );
    return;
  }
  let Some((port, hits)) = start_server() else {
    check("navigation: a loopback server for the checks", false);
    return;
  };
  let own = format!("http://127.0.0.1:{port}");
  let other = format!("http://localhost:{port}");
  let seen = Mutex::new(Vec::<String>::new());

  let win = Window::new(480, 320).load(&format!("{own}/"));
  let first = wait_location(&win, &format!("{own}/"), 100).await;
  check(
    &format!("navigation: the embedder's own navigation loads ({first:?})"),
    first.as_deref() == Some(format!("{own}/").as_str()),
  );

  // Same origin.
  eval_string(&win, "location.href = '/second'; 'ok'").await;
  let second = wait_location(&win, &format!("{own}/second"), 50).await;
  check(
    &format!("navigation: a same-origin navigation loads ({second:?})"),
    second.as_deref() == Some(format!("{own}/second").as_str()),
  );

  // Another origin, from a script (no user gesture).
  let away = format!("{other}/away");
  eval_string(&win, &format!("location.href = '{away}'; 'ok'")).await;
  let opened = wait_captured(&seen, &away).await;
  let stayed = location(&win).await;
  check(
    &format!(
      "navigation: another origin does not load in the window and goes to \
       the browser instead ({stayed:?}, captured {:?})",
      seen.lock().unwrap()
    ),
    opened && stayed.as_deref() == Some(format!("{own}/second").as_str()),
  );

  // A meta refresh to another origin: no user gesture and no page-side
  // interceptor in the way, so the native guard alone decides.
  let refreshed = format!("{other}/refreshed");
  eval_string(
    &win,
    &format!(
      "var m = document.createElement('meta'); m.httpEquiv = 'refresh'; \
       m.content = '0;url={refreshed}'; document.head.appendChild(m); 'ok'"
    ),
  )
  .await;
  let opened = wait_captured(&seen, &refreshed).await;
  let stayed = location(&win).await;
  check(
    &format!(
      "navigation: a meta refresh to another origin does not load either \
       ({stayed:?})"
    ),
    opened && stayed.as_deref() == Some(format!("{own}/second").as_str()),
  );

  // A same-origin request the server redirects to another origin.
  let redirected = format!("{other}/redirected");
  eval_string(&win, "location.href = '/redirect'; 'ok'").await;
  let opened = wait_captured(&seen, &redirected).await;
  tokio::time::sleep(Duration::from_millis(500)).await;
  let stayed = location(&win).await;
  check(
    &format!(
      "navigation: a redirect to another origin does not load either \
       ({stayed:?}, /redirect asked: {})",
      hits.lock().unwrap().iter().any(|p| p == "/redirect")
    ),
    opened && stayed.as_deref() == Some(format!("{own}/second").as_str()),
  );

  // window.open(): never a web view. A script's call has no user gesture,
  // though an engine may count the embedder's own script evaluation as one,
  // in which case the URL goes to the browser (captured). Either way the
  // window keeps its document and no popup ever asks the server for it.
  let popup = format!("{other}/popup");
  eval_string(&win, &format!("window.open('{popup}'); 'ok'")).await;
  tokio::time::sleep(Duration::from_millis(1500)).await;
  if let Some(more) = laufey::test_take_external_opens() {
    seen.lock().unwrap().extend(more);
  }
  let stayed = location(&win).await;
  check(
    &format!(
      "navigation: window.open() opens no web view ({stayed:?}, to the \
       browser: {})",
      seen.lock().unwrap().iter().any(|u| u == &popup)
    ),
    !hits.lock().unwrap().iter().any(|p| p == "/popup")
      && stayed.as_deref() == Some(format!("{own}/second").as_str()),
  );

  // A page navigating away in a loop: at most 3 a second reach the browser.
  tokio::time::sleep(Duration::from_millis(1100)).await;
  let _ = laufey::test_take_external_opens();
  eval_string(
    &win,
    &format!(
      "for (let i = 0; i < 12; i++) setTimeout(() => {{ \
       location.href = '{other}/burst' + i; }}, i * 40); 'ok'"
    ),
  )
  .await;
  tokio::time::sleep(Duration::from_millis(1500)).await;
  let burst = laufey::test_take_external_opens()
    .unwrap_or_default()
    .into_iter()
    .filter(|u| u.contains("/burst"))
    .count();
  let stayed = location(&win).await;
  check(
    &format!(
      "navigation: a navigation loop reaches the browser at most 3 times a \
       second ({burst} opened, {stayed:?})"
    ),
    (1..=4).contains(&burst)
      && stayed.as_deref() == Some(format!("{own}/second").as_str()),
  );

  // A sub-frame on another origin is the page's CSP's business.
  eval_string(
    &win,
    &format!(
      "var f = document.createElement('iframe'); f.src = '{other}/frame'; \
       document.body.appendChild(f); 'ok'"
    ),
  )
  .await;
  let framed = wait_for(
    || hits.lock().unwrap().iter().any(|p| p == "/frame"),
    50,
    200,
  )
  .await;
  tokio::time::sleep(Duration::from_millis(500)).await;
  let stayed = location(&win).await;
  check(
    &format!(
      "navigation: an iframe on another origin still loads ({stayed:?})"
    ),
    framed && stayed.as_deref() == Some(format!("{own}/second").as_str()),
  );

  refused_navigation_keeps_page(&own, &other, &hits).await;

  // An origin the embedder allows.
  laufey::set_allowed_navigation_origins(&[format!("{other}/")]);
  let allowed = format!("{other}/allowed");
  eval_string(&win, &format!("location.href = '{allowed}'; 'ok'")).await;
  let now = wait_location(&win, &allowed, 50).await;
  check(
    &format!("navigation: an allowed origin loads ({now:?})"),
    now.as_deref() == Some(allowed.as_str()),
  );

  let empty: [&str; 0] = [];
  laufey::set_allowed_navigation_origins(&empty);
  laufey::test_capture_external_opens(false);
  let _ = &win;
}

/// The page a refused navigation must leave alive: a counter its timer
/// bumps (scripts keep running).
const LIVE_PAGE: &str = "<!doctype html><html><head><meta charset=\"utf-8\">\
<title>live</title></head><body>live<script>window.ticks = 0; \
setInterval(() => { window.ticks++; }, 50);</script></body></html>";

/// The custom-scheme page of the refused-navigation checks (an app's own
/// origin, as a packaged app's page is).
const LIVE_SCHEME_URL: &str = "app://e2e-nav-live/live";

/// How long the will-navigate handler takes to answer a `-late` URL: the
/// backend holds the navigation meanwhile, and the page calls the bridge
/// and fetches.
const ANSWER_DELAY: Duration = Duration::from_millis(1200);

/// How long it takes to answer a `-soon` URL (the loop's late answers).
const SOON_DELAY: Duration = Duration::from_millis(200);

/// How many refused navigations the loop makes, each page's half in a row.
const LOOP_ROUNDS: usize = 30;

/// Serves the refused-navigation checks' custom-scheme page (and its
/// `fetched` path); hands every other request back.
pub fn serve_scheme(
  req: laufey::SchemeRequest,
) -> Option<laufey::SchemeRequest> {
  let path = req.url.split(['?', '#']).next().unwrap_or("").to_string();
  match path.as_str() {
    LIVE_SCHEME_URL => {
      super::respond(
        req,
        200,
        "text/html; charset=utf-8",
        LIVE_PAGE.as_bytes(),
      );
      None
    }
    "app://e2e-nav-live/fetched" => {
      super::respond(req, 200, "text/plain", b"fetched");
      None
    }
    _ => Some(req),
  }
}

/// API 56: a navigation the will-navigate handler refuses leaves the page
/// alive, from the loopback server's page and from a custom-scheme one.
async fn refused_navigation_keeps_page(own: &str, other: &str, hits: &Hits) {
  let reported: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
  let seen = reported.clone();
  let refused_prefix = format!("{other}/refused");
  if !laufey::set_will_navigate_handler(move |nav| {
    seen
      .lock()
      .unwrap()
      .push(format!("{} {}", nav.kind.as_str(), nav.url));
    let answer = if nav.url.starts_with(&refused_prefix) {
      NavigationAnswer::Refuse
    } else if nav.url.contains("/click-allow") {
      NavigationAnswer::Allow
    } else {
      NavigationAnswer::Default
    };
    // From another thread: at once (as an embedder whose own thread decides
    // synchronously), or after ANSWER_DELAY (an async decision).
    let id = nav.id;
    let delay = if nav.url.ends_with("-now") {
      Duration::ZERO
    } else if nav.url.ends_with("-soon") {
      SOON_DELAY
    } else {
      ANSWER_DELAY
    };
    std::thread::spawn(move || {
      std::thread::sleep(delay);
      laufey::answer_will_navigate(id, answer);
    });
  }) {
    na("navigation: refused will-navigate (backend has no will-navigate hook)");
    return;
  }
  for mode in ["now", "late"] {
    refused_from(&format!("{own}/live"), "http", mode, other, &reported, hits)
      .await;
    refused_from(LIVE_SCHEME_URL, "app", mode, other, &reported, hits).await;
  }
  refused_navigation_loop(own, other, &reported).await;
  open_external_asks_first(own, other, &reported).await;
  for answer in ["refuse", "allow", "default"] {
    clicked_link(own, other, answer, &reported).await;
  }
  laufey::clear_will_navigate_handler();
}

/// The external-link interceptor's reserved bridge call (what a user's click
/// on a link to another origin, or on WebKit a script the embedder runs,
/// turns into before the engine sees a navigation) asks will-navigate first
/// too: a refused URL is neither loaded nor handed to the browser.
async fn open_external_asks_first(
  own: &str,
  other: &str,
  reported: &Mutex<Vec<String>>,
) {
  let start = format!("{own}/live");
  let refused = format!("{other}/refused-external-now");
  let win = Window::new(480, 320)
    .bind("ping", |call| call.resolve(Value::String("pong".into())))
    .load(&start);
  let first = wait_url(&win, &start, 100).await;
  let _ = laufey::test_take_external_opens();
  let called = eval_async(
    &win,
    &format!(
      "typeof Laufey === 'undefined' ? 'no bridge' : \
       Promise.resolve(Laufey.__laufeyOpenExternal('{refused}')) \
         .then(() => 'called')"
    ),
  )
  .await;
  let asked = wait_for(
    || {
      reported
        .lock()
        .unwrap()
        .iter()
        .any(|u| u.ends_with(&refused))
    },
    50,
    200,
  )
  .await;
  tokio::time::sleep(Duration::from_millis(500)).await;
  let here = eval_string(&win, "location.href").await;
  let opened = laufey::test_take_external_opens().unwrap_or_default();
  check(
    &format!(
      "navigation: the external-link call asks will-navigate first, and a \
       refused URL neither loads nor reaches the browser ({first:?}, \
       {called:?}, asked {asked}, at {here:?}, to the browser: {opened:?})"
    ),
    first.as_deref() == Some(start.as_str())
      && called.as_deref() == Some("called")
      && asked
      && here.as_deref() == Some(start.as_str())
      && !opened.contains(&refused),
  );
  win.close();
}

/// A real click on a link to another origin, for each answer: the page
/// starts the navigation the way a user does (OS input: xdotool / XTEST on
/// X11, SendInput on Windows; on macOS, where posting OS events needs a
/// permission a CI runner lacks, NSEvents through the app's own event
/// dispatch, which reaches the page as a trusted click). Will-navigate must
/// see it, and then Refuse keeps the page and opens nothing, Allow loads it
/// in the window, Default leaves it to the guard (another origin: the
/// browser, not the window).
async fn clicked_link(
  own: &str,
  other: &str,
  answer: &str,
  reported: &Mutex<Vec<String>>,
) {
  let start = format!("{own}/live");
  let target = match answer {
    "refuse" => format!("{other}/refused-click-now"),
    "allow" => format!("{other}/click-allow-now"),
    _ => format!("{other}/click-default-now"),
  };
  let label = format!("navigation (a clicked link, {answer})");
  let title = format!("native-e2e-click-{answer}");
  let win = Window::new(480, 320).title(&title).load(&start);
  place_clear_of_popups(&win);
  let first = wait_url(&win, &start, 100).await;
  if first.as_deref() != Some(start.as_str()) {
    check(&format!("{label}: the page loads ({first:?})"), false);
    win.close();
    return;
  }
  // A link over the whole page, so the click lands on it wherever the
  // window's content is; the page counts the trusted mousedowns it gets.
  eval_string(
    &win,
    &format!(
      "document.title = '{title}'; window.__downs = 0; \
       addEventListener('mousedown', \
         (e) => {{ if (e.isTrusted) window.__downs++; }}, true); \
       const a = document.createElement('a'); a.href = '{target}'; \
       a.textContent = 'go'; a.style.cssText = 'position:fixed;inset:0;\
         display:block;background:#fff;z-index:2147483647'; \
       document.body.appendChild(a); 'ok'"
    ),
  )
  .await;
  win.show();
  win.focus();
  tokio::time::sleep(Duration::from_millis(500)).await;
  let _ = laufey::test_take_external_opens();
  let Some(how) = real_click(&win, &title).await else {
    win.close();
    return;
  };
  let asked = wait_for(
    || {
      reported
        .lock()
        .unwrap()
        .iter()
        .any(|u| u.ends_with(&target))
    },
    50,
    200,
  )
  .await;
  let downs = eval_string(&win, "String(window.__downs)").await;
  let want = if answer == "allow" { &target } else { &start };
  let here = wait_url(&win, want, 25).await;
  tokio::time::sleep(Duration::from_millis(300)).await;
  let opened = laufey::test_take_external_opens().unwrap_or_default();
  let to_browser = opened.contains(&target);
  let outcome = match answer {
    "refuse" => here.as_deref() == Some(start.as_str()) && !to_browser,
    "allow" => here.as_deref() == Some(target.as_str()) && !to_browser,
    _ => here.as_deref() == Some(start.as_str()) && to_browser,
  };
  check(
    &format!(
      "{label}: will-navigate sees the click's navigation and the answer \
       holds ({how}; trusted mousedowns {downs:?}, asked {asked}, at \
       {here:?}, to the browser: {opened:?})"
    ),
    asked && outcome,
  );
  win.close();
}

/// Puts a clicked-link window in the upper right of the primary display's
/// work area (Windows): system sign-in prompts open in the middle of the
/// screen, and the Start menu, search and the notification centre open from
/// the taskbar at the bottom, so a stray one doesn't sit on the click point.
fn place_clear_of_popups(win: &Window) {
  if !cfg!(windows) {
    return;
  }
  let screens = laufey::screens();
  let Some(screen) = screens.iter().find(|s| s.is_primary).or(screens.first())
  else {
    return;
  };
  let area = screen.work_area;
  win.set_position((area.x + area.width - 480 - 24).max(area.x), area.y + 24);
}

/// Another process's window over the click point, on a CI runner only.
#[cfg(windows)]
mod covering {
  use std::ffi::c_void;

  #[link(name = "user32")]
  extern "system" {
    fn GetWindowThreadProcessId(hwnd: *mut c_void, pid: *mut u32) -> u32;
    fn PostMessageW(hwnd: *mut c_void, msg: u32, wp: usize, lp: isize) -> i32;
    fn IsWindow(hwnd: *mut c_void) -> i32;
  }

  #[link(name = "kernel32")]
  extern "system" {
    fn OpenProcess(access: u32, inherit: i32, pid: u32) -> *mut c_void;
    fn CloseHandle(h: *mut c_void) -> i32;
    fn QueryFullProcessImageNameW(
      process: *mut c_void,
      flags: u32,
      name: *mut u16,
      size: *mut u32,
    ) -> i32;
    fn TerminateProcess(process: *mut c_void, code: u32) -> i32;
  }

  const WM_CLOSE: u32 = 0x0010;
  const PROCESS_TERMINATE: u32 = 0x0001;
  const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x1000;

  /// The shell's own flyouts (Start, search, the notification centre and
  /// quick settings, widgets): dismissed with Escape, as a user does, never
  /// closed or ended.
  const SHELL_FLYOUTS: &[&str] = &[
    "startmenuexperiencehost.exe",
    "searchhost.exe",
    "searchapp.exe",
    "shellexperiencehost.exe",
    "shellhost.exe",
    "widgets.exe",
    "webexperiencehostapp.exe",
  ];

  /// Processes whose window over the test may be ended: the account /
  /// credential brokers and settings hosts a runner image shows prompts
  /// from. Matched on the executable's file name, any case.
  const BROKERS: &[&str] = &[
    "microsoft.aad.brokerplugin.exe",
    "credentialuibroker.exe",
    "systemsettings.exe",
    "applicationframehost.exe",
    "useroobebroker.exe",
  ];

  /// Only on a CI runner (GITHUB_ACTIONS) or when asked for
  /// (LAUFEY_E2E_CLOSE_COVERING_WINDOWS=1): never on a developer's desktop.
  pub fn allowed() -> bool {
    std::env::var("LAUFEY_E2E_CLOSE_COVERING_WINDOWS").as_deref() == Ok("1")
      || std::env::var("GITHUB_ACTIONS").as_deref() == Ok("true")
  }

  fn image_name(pid: u32) -> String {
    unsafe {
      let h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
      if h.is_null() {
        return String::new();
      }
      let mut buf = [0u16; 512];
      let mut len = buf.len() as u32;
      let ok = QueryFullProcessImageNameW(h, 0, buf.as_mut_ptr(), &mut len);
      CloseHandle(h);
      if ok == 0 {
        return String::new();
      }
      String::from_utf16_lossy(&buf[..len as usize])
    }
  }

  /// Closes the top-level window `root` of another process; if it is still
  /// there after a moment and its process is a known broker, ends that
  /// process. Logs what it did.
  pub fn clear(root: *mut c_void) {
    let mut pid = 0u32;
    unsafe { GetWindowThreadProcessId(root, &mut pid) };
    if pid == 0 || pid == std::process::id() {
      return;
    }
    let image = image_name(pid);
    let file = image
      .rsplit(['\\', '/'])
      .next()
      .unwrap_or("")
      .to_ascii_lowercase();
    let what = crate::io_checks::ole_input::window_at_hwnd(root);
    if SHELL_FLYOUTS.contains(&file.as_str()) {
      eprintln!(
        "[e2e] INFO dismissing {what} ({image}) over the test window with \
         Escape"
      );
      crate::menu_notification_checks::win::press(
        crate::menu_notification_checks::win::VK_ESCAPE,
      );
      std::thread::sleep(std::time::Duration::from_millis(600));
      return;
    }
    eprintln!("[e2e] INFO closing {what} ({image}) over the test window");
    unsafe { PostMessageW(root, WM_CLOSE, 0, 0) };
    std::thread::sleep(std::time::Duration::from_millis(800));
    if unsafe { IsWindow(root) } == 0 {
      eprintln!("[e2e] INFO it closed");
      return;
    }
    if !BROKERS.contains(&file.as_str()) {
      eprintln!("[e2e] INFO it stayed; {file:?} is not a known broker");
      return;
    }
    let ended = unsafe {
      let h = OpenProcess(PROCESS_TERMINATE, 0, pid);
      let ok = !h.is_null() && TerminateProcess(h, 1) != 0;
      if !h.is_null() {
        CloseHandle(h);
      }
      ok
    };
    std::thread::sleep(std::time::Duration::from_millis(500));
    eprintln!("[e2e] INFO it stayed; ended {file} (pid {pid}): {ended}");
  }
}

/// Clicks the middle of the window's content with real input; how, or
/// `None` (reported) when this session can't.
async fn real_click(win: &Window, title: &str) -> Option<String> {
  #[cfg(target_os = "macos")]
  {
    let (w, h) = win.get_size();
    let t = title.to_string();
    let (x, y) = (w as f64 / 2.0, h as f64 / 2.0);
    let ok = laufey::spawn_on_ui_thread(move || {
      crate::overlay_checks::mac::app_click(&t, x, y)
    })
    .await
    .unwrap_or(false);
    if !ok {
      check(
        &format!("a clicked link: NSEvents reach the window {title:?}"),
        false,
      );
      return None;
    }
    Some("NSEvents through -[NSApplication sendEvent:]".into())
  }
  #[cfg(not(target_os = "macos"))]
  {
    let _ = win;
    if cfg!(target_os = "linux") && crate::os_view::xdotool().is_none() {
      na("a clicked link (needs xdotool on X11)");
      return None;
    }
    let Some((x, y, w, h)) = crate::os_view::content_rect(title) else {
      check(
        &format!("a clicked link: the window {title:?} is on screen"),
        false,
      );
      return None;
    };
    let (cx, cy) = (x + w / 2, y + h / 2);
    #[cfg(windows)]
    {
      use crate::io_checks::ole_input;
      use crate::menu_notification_checks::win;
      // In front of every other window for the click (a runner image may
      // show a system window, such as a Microsoft account prompt, over the
      // test window), then back to normal: the foreground (sharing the
      // foreground thread's input, else an Alt press), and the topmost band.
      let hwnd = win::find_window(title);
      if hwnd.is_null() {
        check(&format!("a clicked link: a window titled {title:?}"), false);
        return None;
      }
      if !win::focus(hwnd) {
        win::activate_with_alt(hwnd);
      }
      let topmost = win::set_topmost(hwnd, true);
      tokio::time::sleep(Duration::from_millis(300)).await;
      let mut at = ole_input::root_at(cx, cy);
      if at != hwnd && !at.is_null() && covering::allowed() {
        // A CI runner's own system window (GitHub's windows-11-arm image
        // shows a Microsoft account sign-in window over everything): closed,
        // and its broker process ended if it stays, then asked again.
        covering::clear(at);
        win::set_topmost(hwnd, true);
        tokio::time::sleep(Duration::from_millis(500)).await;
        at = ole_input::root_at(cx, cy);
      }
      if at != hwnd {
        let covered_by = ole_input::window_at(cx, cy);
        win::set_topmost(hwnd, false);
        check(
          &format!(
            "a clicked link: the test window is under the click point \
             ({cx}, {cy}) (topmost {topmost}; there: {covered_by})"
          ),
          false,
        );
        return None;
      }
      ole_input::move_to(cx, cy);
      tokio::time::sleep(Duration::from_millis(80)).await;
      // Again just before the press: a shell flyout can open meanwhile (the
      // runner's Start menu did once, right after a sign-in prompt closed).
      for _ in 0..3 {
        let at = ole_input::root_at(cx, cy);
        if at == hwnd || at.is_null() || !covering::allowed() {
          break;
        }
        covering::clear(at);
        win::set_topmost(hwnd, true);
        tokio::time::sleep(Duration::from_millis(300)).await;
        ole_input::move_to(cx, cy);
        tokio::time::sleep(Duration::from_millis(80)).await;
      }
      if ole_input::root_at(cx, cy) != hwnd {
        let covered_by = ole_input::window_at(cx, cy);
        win::set_topmost(hwnd, false);
        check(
          &format!(
            "a clicked link: the test window is under the click point \
             ({cx}, {cy}) when it is pressed (there: {covered_by})"
          ),
          false,
        );
        return None;
      }
      ole_input::button(true);
      tokio::time::sleep(Duration::from_millis(60)).await;
      ole_input::button(false);
      let over = ole_input::window_at(cx, cy);
      win::set_topmost(hwnd, false);
      Some(format!("SendInput at ({cx}, {cy}), over {over}"))
    }
    #[cfg(not(windows))]
    {
      let (sx, sy) = (cx.to_string(), cy.to_string());
      let moved = crate::os_view::xdo(&["mousemove", &sx, &sy]).is_some();
      tokio::time::sleep(Duration::from_millis(80)).await;
      let clicked = crate::os_view::xdo(&["click", "1"]).is_some();
      Some(format!(
        "xdotool at ({cx}, {cy}): moved {moved}, clicked {clicked}"
      ))
    }
  }
}

/// The refusal again and again, from each page: every round's navigation
/// is refused at once or a moment later while the page fetches and calls
/// the bridge, and every round must leave the page where it was, its
/// fetches loaded, its bridge calls answered and its timers running. One
/// lost round fails the check (a race, not a rule, so it has to hold every
/// time).
async fn refused_navigation_loop(
  own: &str,
  other: &str,
  reported: &Mutex<Vec<String>>,
) {
  let _ = laufey::test_take_external_opens();
  let mut failures: Vec<String> = Vec::new();
  let mut rounds = 0;
  let pages = [
    ("http", format!("{own}/live")),
    ("app", LIVE_SCHEME_URL.to_string()),
  ];
  for (label, start) in pages {
    let win = Window::new(480, 320)
      .bind("ping", |call| call.resolve(Value::String("pong".into())))
      .load(&start);
    let first = wait_url(&win, &start, 100).await;
    if first.as_deref() != Some(start.as_str()) {
      failures.push(format!("{label}: the page never loaded ({first:?})"));
      win.close();
      continue;
    }
    for round in 0..LOOP_ROUNDS / 2 {
      rounds += 1;
      let mode = if round % 2 == 0 { "now" } else { "soon" };
      let refused = format!("{other}/refused-loop-{label}-{round}-{mode}");
      let before = eval_string(&win, "String(window.ticks)").await;
      // Navigate, and at once (and again 20 ms later) fetch and call the
      // bridge; each result lands in window.__loop.
      eval_string(
        &win,
        &format!(
          "(() => {{ window.__loop = []; \
             const note = (p) => {{ const i = window.__loop.length; \
               window.__loop.push('pending'); \
               p.then((v) => {{ window.__loop[i] = String(v); }}, \
               (e) => {{ window.__loop[i] = 'failed ' + e; }}); }}; \
             const get = (q) => fetch('fetched?{round}' + q, \
               {{ cache: 'no-store' }}).then((r) => 'status ' + r.status); \
             const ping = () => typeof Laufey === 'undefined' \
               ? Promise.resolve('no bridge') : Laufey.ping(); \
             location.href = '{refused}'; note(get('a')); note(ping()); \
             setTimeout(() => {{ note(get('b')); note(ping()); }}, 20); \
           }})(); 'ok'"
        ),
      )
      .await;
      let asked = wait_for(
        || {
          reported
            .lock()
            .unwrap()
            .iter()
            .any(|u| u.ends_with(&refused))
        },
        50,
        200,
      )
      .await;
      if mode == "soon" {
        tokio::time::sleep(SOON_DELAY).await;
      }
      // Every result in, then the page's state.
      let mut results = None;
      for _ in 0..40 {
        results = eval_string(&win, "JSON.stringify(window.__loop)").await;
        let settled = results
          .as_deref()
          .and_then(|r| serde_json::from_str::<Vec<String>>(r).ok())
          .is_some_and(|v| v.len() == 4 && v.iter().all(|r| r != "pending"));
        if settled {
          break;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
      }
      let here = eval_string(&win, "location.href").await;
      tokio::time::sleep(Duration::from_millis(150)).await;
      let after = eval_string(&win, "String(window.ticks)").await;
      let ticking = matches!(
        (before.as_deref().and_then(|v| v.parse::<u64>().ok()),
         after.as_deref().and_then(|v| v.parse::<u64>().ok())),
        (Some(a), Some(b)) if b > a
      );
      let loaded: Option<Vec<String>> = results
        .as_deref()
        .and_then(|r| serde_json::from_str(r).ok());
      let all_loaded = loaded.as_ref().is_some_and(|v| {
        v.len() == 4 && v.iter().all(|r| r == "status 200" || r == "pong")
      });
      if !(asked
        && here.as_deref() == Some(start.as_str())
        && ticking
        && all_loaded)
      {
        failures.push(format!(
          "{label} round {round} ({mode}): asked {asked}, at {here:?}, \
           ticks {before:?} -> {after:?}, results {results:?}"
        ));
      }
    }
    win.close();
  }
  let opened = laufey::test_take_external_opens().unwrap_or_default();
  check(
    &format!(
      "navigation: {rounds} refused navigations in a row each keep the page \
       with its fetches, bridge and timers, and open nothing ({} lost: \
       {failures:?}; to the browser: {opened:?})",
      failures.len()
    ),
    rounds == LOOP_ROUNDS && failures.is_empty() && opened.is_empty(),
  );
}

/// One page's run: `start` loads and navigates to the refused URL, calling
/// the bridge and fetching while the navigation is pending; then the page
/// must still be there, run its timers, answer bridge calls (as its own
/// origin) and load fetches.
async fn refused_from(
  start: &str,
  label: &str,
  mode: &str,
  other: &str,
  reported: &Mutex<Vec<String>>,
  hits: &Hits,
) {
  let refused = format!("{other}/refused-{label}-{mode}");
  let label = format!("{label}, answered {mode}");
  // The origin each bridge call carried (call.origin): the page's, during
  // the held navigation and after it.
  let pings: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
  let counted = pings.clone();
  let win = Window::new(480, 320)
    .bind("ping", move |call| {
      counted.lock().unwrap().push(call.origin.clone());
      call.resolve(Value::String("pong".into()));
    })
    .load(start);
  let first = wait_url(&win, start, 100).await;
  let bridged = bridge_answers(&win).await;
  check(
    &format!(
      "navigation ({label}): will-navigate page loads and its bridge answers \
       ({first:?}, {bridged:?})"
    ),
    first.as_deref() == Some(start) && bridged.as_deref() == Some("pong"),
  );

  let _ = laufey::test_take_external_opens();
  // Navigate, and at once call the bridge and fetch; again 300 ms later,
  // inside the hold when the answer comes late.
  eval_string(
    &win,
    &format!(
      "const settle = (p, key) => {{ window[key] = 'pending'; \
         p.then((v) => {{ window[key] = String(v); }}, \
         (e) => {{ window[key] = 'failed ' + e; }}); }}; \
       const ping = () => typeof Laufey === 'undefined' \
         ? Promise.resolve('no bridge') : Laufey.ping(); \
       const get = (q) => fetch('fetched?' + q, {{ cache: 'no-store' }}) \
         .then((r) => 'status ' + r.status); \
       const post = (q) => fetch('fetched?' + q, {{ method: 'POST', \
         cache: 'no-store', body: '{{}}', headers: \
         {{ 'content-type': 'application/json', 'x-probe': '1' }} }}) \
         .then((r) => 'status ' + r.status); \
       location.href = '{refused}'; \
       settle(ping(), 'startPing'); settle(get('start'), 'startFetch'); \
       setTimeout(() => {{ settle(ping(), 'heldPing'); \
         settle(get('held'), 'heldFetch'); \
         settle(post('held-post'), 'heldPost'); }}, 300); 'ok'"
    ),
  )
  .await;
  let reached = wait_for(
    || {
      reported
        .lock()
        .unwrap()
        .iter()
        .any(|u| u.ends_with(&refused))
    },
    50,
    200,
  )
  .await;
  check(
    &format!(
      "navigation ({label}): the will-navigate handler sees the page's \
       navigation ({:?})",
      reported.lock().unwrap()
    ),
    reached,
  );
  tokio::time::sleep(ANSWER_DELAY + Duration::from_millis(1500)).await;

  let stayed = eval_string(&win, "location.href").await;
  let opened = laufey::test_take_external_opens().unwrap_or_default();
  check(
    &format!(
      "navigation ({label}): a refused navigation keeps the page and opens \
       nothing ({stayed:?}, to the browser: {opened:?})"
    ),
    stayed.as_deref() == Some(start) && !opened.contains(&refused),
  );

  // Scripts still run: the page's own timer keeps counting.
  let t1 = eval_string(&win, "String(window.ticks)").await;
  tokio::time::sleep(Duration::from_millis(400)).await;
  let t2 = eval_string(&win, "String(window.ticks)").await;
  let ticking = matches!(
    (t1.as_deref().and_then(|v| v.parse::<u64>().ok()),
     t2.as_deref().and_then(|v| v.parse::<u64>().ok())),
    (Some(a), Some(b)) if b > a
  );
  check(
    &format!(
      "navigation ({label}): after a refused navigation the page's timers \
       still run ({t1:?} -> {t2:?})"
    ),
    ticking,
  );

  // What the page started while the navigation was pending: its bridge
  // calls are answered and its fetches load (the navigation never started:
  // WebKitGTK holds a frameless document's navigation at its action, before
  // the engine starts the load and the document's own loads fail).
  let during = eval_string(
    &win,
    "JSON.stringify([window.startPing, window.heldPing, window.startFetch, \
     window.heldFetch, window.heldPost])",
  )
  .await;
  let settled: Option<Vec<String>> =
    during.as_deref().and_then(|d| serde_json::from_str(d).ok());
  check(
    &format!(
      "navigation ({label}): the bridge calls made while the navigation was \
       pending are answered, and its fetches load ({during:?})"
    ),
    settled.as_ref().is_some_and(|v| {
      v.len() == 5
        && v[0] == "pong"
        && v[1] == "pong"
        && v[2..].iter().all(|f| f == "status 200")
    }),
  );

  // The bridge: a call the page makes now is answered.
  let bridged = bridge_answers(&win).await;
  let origin = laufey_origin(start);
  let origins = pings.lock().unwrap().clone();
  check(
    &format!(
      "navigation ({label}): after a refused navigation the page's bridge \
       calls are answered, every one as the page's origin {origin} \
       ({bridged:?}, origins {origins:?})"
    ),
    bridged.as_deref() == Some("pong")
      && origins.len() >= 3
      && origins.iter().all(|o| *o == origin),
  );

  // The network: a fetch the page starts now loads.
  let fetched = eval_async(
    &win,
    "fetch('fetched', { cache: 'no-store' }).then((r) => 'status ' + r.status, \
     (e) => 'failed ' + e)",
  )
  .await;
  check(
    &format!(
      "navigation ({label}): after a refused navigation the page's fetches \
       load ({fetched:?}, the server asked for {refused}: {})",
      hits
        .lock()
        .unwrap()
        .iter()
        .any(|p| refused.ends_with(p.as_str()))
    ),
    fetched.as_deref() == Some("status 200"),
  );

  win.close();
}

/// The origin of `url` as the bridge reports it (`scheme://host[:port]`).
fn laufey_origin(url: &str) -> String {
  match url.find("://") {
    Some(i) => {
      let rest = &url[i + 3..];
      let host = rest.split(['/', '?', '#']).next().unwrap_or("");
      format!("{}://{host}", &url[..i])
    }
    None => url.to_string(),
  }
}

/// Polls `location.href` until it is `want` (or about `tries` × 200 ms
/// pass).
async fn wait_url(win: &Window, want: &str, tries: u32) -> Option<String> {
  let mut last = None;
  for _ in 0..tries {
    last = eval_string(win, "location.href").await;
    if last.as_deref() == Some(want) {
      break;
    }
    tokio::time::sleep(Duration::from_millis(200)).await;
  }
  last
}

/// Starts `promise` (a script whose value is a promise of a string) in the
/// page and polls for what it settled to.
async fn eval_async(win: &Window, promise: &str) -> Option<String> {
  eval_string(
    win,
    &format!(
      "window.__settled = undefined; Promise.resolve().then(() => {promise})\
       .then((v) => {{ window.__settled = String(v); }}, \
       (e) => {{ window.__settled = 'rejected ' + e; }}); 'ok'"
    ),
  )
  .await?;
  for _ in 0..50 {
    let v = eval_string(
      win,
      "window.__settled === undefined ? '' : String(window.__settled)",
    )
    .await;
    if v.as_deref().is_some_and(|v| !v.is_empty()) {
      return v;
    }
    tokio::time::sleep(Duration::from_millis(200)).await;
  }
  None
}

/// A bridge call from the page (`Laufey.ping()`): what it resolved to.
async fn bridge_answers(win: &Window) -> Option<String> {
  eval_async(
    win,
    "typeof Laufey === 'undefined' ? 'no bridge' : Laufey.ping()",
  )
  .await
}
