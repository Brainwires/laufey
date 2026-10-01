//! Per-app web storage e2e (LAUFEY_DATA_DIR / LAUFEY_APP_ID).
//!
//! One launch of this runtime is one step of the scenario driven by
//! `scripts/storage-e2e-run.sh`, which relaunches the backend with different
//! app ids / data dirs and asserts that storage persists per app and stays
//! isolated between apps. Each launch serves a page on a fixed loopback port
//! (so every launch sees the same origin), loads it, and then, by
//! `LAUFEY_E2E_STORAGE_MODE`:
//!
//!   write  stores `LAUFEY_E2E_STORAGE_VALUE` in localStorage and in a
//!          persistent cookie, checks it reads back, then quits normally so
//!          the engine flushes its profile to disk.
//!   read   reads both back and checks them against
//!          `LAUFEY_E2E_STORAGE_EXPECT` (must equal) and/or
//!          `LAUFEY_E2E_STORAGE_EXPECT_NOT` (must not equal).
//!
//! `LAUFEY_E2E_STORAGE_HOLD_MS` keeps the launch alive that long before it
//! quits (used to start a second instance against a running one).
//!
//! Emits `[e2e] PASS/FAIL <name>` lines and a final `[e2e] OVERALL PASS|FAIL`.
//! The process exits through the backend's normal quit path (which owns the
//! exit code), so the driver judges each step by the OVERALL line.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use laufey::{Value, Window};
use tokio::sync::oneshot;

static FAILED: AtomicBool = AtomicBool::new(false);

const KEY: &str = "laufey-e2e-storage";
const COOKIE: &str = "laufey_e2e_storage";

fn check(name: &str, ok: bool) {
  if ok {
    eprintln!("[e2e] PASS {name}");
  } else {
    eprintln!("[e2e] FAIL {name}");
    FAILED.store(true, Ordering::SeqCst);
  }
}

fn env(name: &str) -> Option<String> {
  std::env::var(name).ok().filter(|v| !v.is_empty())
}

/// Serves the same tiny page for every request on 127.0.0.1:`port`.
///
/// Each connection gets its own thread. A persistent profile remembers this
/// origin, so on a relaunch Chromium's loading predictor preconnects idle
/// sockets to it alongside the navigation's own connection; a server that
/// handles one connection at a time can block reading an idle preconnect and
/// never answer the request waiting on the other socket.
fn serve(port: u16) -> std::io::Result<()> {
  let listener = TcpListener::bind(("127.0.0.1", port))?;
  std::thread::spawn(move || {
    for stream in listener.incoming() {
      let Ok(stream) = stream else { continue };
      std::thread::spawn(move || answer(stream));
    }
  });
  Ok(())
}

/// Reads one request's headers from `stream` and answers with the page. A
/// connection that sends nothing (an unused preconnect) is dropped once the
/// peer closes it or the read times out.
fn answer(mut stream: TcpStream) {
  let _ = stream.set_read_timeout(Some(Duration::from_secs(30)));
  let mut buf = [0u8; 4096];
  let mut req = Vec::new();
  loop {
    match stream.read(&mut buf) {
      Ok(0) | Err(_) => break,
      Ok(n) => req.extend_from_slice(&buf[..n]),
    }
    if req.windows(4).any(|w| w == b"\r\n\r\n") || req.len() > 65536 {
      break;
    }
  }
  if req.is_empty() {
    return;
  }
  let body = "<!doctype html><title>storage-e2e</title><p>storage-e2e</p>";
  let _ = write!(
    stream,
    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\
     Content-Length: {}\r\nCache-Control: no-store\r\n\
     Connection: close\r\n\r\n{body}",
    body.len()
  );
}

/// Runs `script` in the page and returns its string result, if any.
async fn eval_string(win: &Window, script: &str) -> Option<String> {
  let (tx, rx) = oneshot::channel::<Result<Value, Value>>();
  win.execute_js(
    script,
    Some(move |r: Result<Value, Value>| {
      let _ = tx.send(r);
    }),
  );
  match tokio::time::timeout(Duration::from_secs(5), rx).await {
    Ok(Ok(Ok(Value::String(s)))) => Some(s),
    _ => None,
  }
}

/// `"<localStorage value>|<cookie value>"`, each "" when absent.
const READ_JS: &str = r#"(() => {
  const m = document.cookie.match(/(?:^|; )laufey_e2e_storage=([^;]*)/);
  return (localStorage.getItem("laufey-e2e-storage") || "") + "|" +
    (m ? decodeURIComponent(m[1]) : "");
})()"#;

fn split(pair: &str) -> (String, String) {
  let mut it = pair.splitn(2, '|');
  let ls = it.next().unwrap_or("").to_string();
  let cookie = it.next().unwrap_or("").to_string();
  (ls, cookie)
}

fn e2e_main() {
  let rt = tokio::runtime::Runtime::new().expect("tokio runtime");
  rt.block_on(async move {
    tokio::spawn(async { laufey::run().await });

    let mode = env("LAUFEY_E2E_STORAGE_MODE").unwrap_or_default();
    let port: u16 = env("LAUFEY_E2E_STORAGE_PORT")
      .and_then(|p| p.parse().ok())
      .unwrap_or(0);
    eprintln!(
      "[e2e] storage mode={mode} port={port} LAUFEY_APP_ID={:?} LAUFEY_DATA_DIR={:?}",
      env("LAUFEY_APP_ID"),
      env("LAUFEY_DATA_DIR")
    );

    let origin = format!("http://127.0.0.1:{port}");
    let served = port != 0 && serve(port).is_ok();
    check(&format!("serve {origin}"), served);

    let win = Window::new(480, 320)
      .title("storage-e2e")
      .load(&format!("{origin}/"));

    // Wait until the page's own origin answers (not about:blank or an error
    // page). Polled rather than via on_page_load, which CEF doesn't fire.
    let mut ready = false;
    if served {
      for _ in 0..100 {
        if eval_string(&win, "location.origin").await.as_deref()
          == Some(origin.as_str())
        {
          ready = true;
          break;
        }
        tokio::time::sleep(Duration::from_millis(200)).await;
      }
    }
    check(&format!("page loaded at {origin}"), ready);

    if ready {
      match mode.as_str() {
        "write" => {
          let value = env("LAUFEY_E2E_STORAGE_VALUE").unwrap_or_default();
          let script = format!(
            r#"(() => {{
  localStorage.setItem("{KEY}", "{value}");
  document.cookie = "{COOKIE}=" + encodeURIComponent("{value}") +
    "; max-age=86400; path=/; samesite=lax";
  return {READ_JS};
}})()"#
          );
          let got = eval_string(&win, &script).await.unwrap_or_default();
          let (ls, cookie) = split(&got);
          eprintln!("[e2e] wrote localStorage={ls:?} cookie={cookie:?}");
          check("localStorage write reads back", !value.is_empty() && ls == value);
          check("cookie write reads back", !value.is_empty() && cookie == value);
          // Give the engine a moment to commit before the normal quit below.
          tokio::time::sleep(Duration::from_millis(1500)).await;
        }
        "read" => {
          let got = eval_string(&win, READ_JS).await;
          check("read storage", got.is_some());
          let (ls, cookie) = split(&got.unwrap_or_default());
          eprintln!("[e2e] read localStorage={ls:?} cookie={cookie:?}");
          if let Some(want) = env("LAUFEY_E2E_STORAGE_EXPECT") {
            check(&format!("localStorage is {want:?}"), ls == want);
            check(&format!("cookie is {want:?}"), cookie == want);
          }
          if let Some(not) = env("LAUFEY_E2E_STORAGE_EXPECT_NOT") {
            check(&format!("localStorage is not {not:?}"), ls != not);
            check(&format!("cookie is not {not:?}"), cookie != not);
          }
        }
        other => check(&format!("known mode (got {other:?})"), false),
      }
    }

    // Stay up (e.g. so the driver can launch a second instance against it).
    if let Some(ms) = env("LAUFEY_E2E_STORAGE_HOLD_MS").and_then(|v| v.parse().ok()) {
      eprintln!("[e2e] holding for {ms} ms");
      tokio::time::sleep(Duration::from_millis(ms)).await;
    }

    let failed = FAILED.load(Ordering::SeqCst);
    eprintln!("[e2e] OVERALL {}", if failed { "FAIL" } else { "PASS" });
    let _ = std::io::stderr().flush();

    // Quit through the backend's normal path so the engine shuts down and
    // flushes storage; returning lets the backend join this thread.
    win.close();
    tokio::time::sleep(Duration::from_millis(300)).await;
    laufey::quit();
  });
}

laufey::main!(e2e_main);
