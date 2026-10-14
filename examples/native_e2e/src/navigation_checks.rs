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
//! - so is a same-origin request the server redirects to the other origin;
//! - a `window.open()` without a user gesture opens nothing anywhere;
//! - an iframe on the other origin still loads (sub-frames are the CSP's);
//! - once the embedder allows the other origin, a navigation there loads.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{Value, Window};

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

  // window.open() with no user gesture: no window, no browser.
  let popup = format!("{other}/popup");
  eval_string(&win, &format!("window.open('{popup}'); 'ok'")).await;
  tokio::time::sleep(Duration::from_millis(1500)).await;
  if let Some(more) = laufey::test_take_external_opens() {
    seen.lock().unwrap().extend(more);
  }
  let stayed = location(&win).await;
  check(
    &format!(
      "navigation: window.open() without a gesture opens nothing ({stayed:?})"
    ),
    !seen.lock().unwrap().iter().any(|u| u == &popup)
      && !hits.lock().unwrap().iter().any(|p| p == "/popup")
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
