// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! CEF on macOS: the cookie key's keychain item (laufey_cef_keychain.h). Run
//! by `scripts/native-e2e-keychain.sh` as several launches of one profile
//! (`LAUFEY_E2E_ONLY=keychain`, a fixed `LAUFEY_APP_ID` and data dir):
//!
//! - `LAUFEY_E2E_KEYCHAIN_PHASE=set`: a loopback page sets a persistent
//!   cookie;
//! - `LAUFEY_E2E_KEYCHAIN_PHASE=check`: a page from the same host sees it
//!   (the profile's cookies decrypted, by whatever key this launch runs
//!   with).
//!
//! Either way the battery reports the host's `cookieEncryption`, which must be
//! `LAUFEY_E2E_KEYCHAIN_EXPECT` ("basic" under the mock keychain, "os" with
//! the app's own item), then quits through laufey so CEF writes its cookie
//! store out.

use std::io::{Read, Write};
use std::net::TcpListener;
use std::time::Duration;

use laufey::{Value, Window};

use super::{check, wait_for};

const COOKIE: &str = "laufey_keychain_e2e=persisted";

fn serve(listener: TcpListener, set_cookie: bool) {
  for stream in listener.incoming().flatten() {
    std::thread::spawn(move || {
      let mut stream = stream;
      let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
      let mut buf = [0u8; 4096];
      let _ = stream.read(&mut buf);
      let body = "<!doctype html><title>keychain</title>keychain";
      let cookie = if set_cookie {
        format!("set-cookie: {COOKIE}; Max-Age=3600; Path=/\r\n")
      } else {
        String::new()
      };
      let _ = stream.write_all(
        format!(
          "HTTP/1.1 200 OK\r\ncontent-type: text/html\r\n{cookie}\
           cache-control: no-store\r\ncontent-length: {}\r\n\
           connection: close\r\n\r\n{body}",
          body.len()
        )
        .as_bytes(),
      );
    });
  }
}

async fn document_cookie(win: &Window) -> Option<String> {
  for _ in 0..50 {
    let (tx, rx) = tokio::sync::oneshot::channel();
    win.execute_js(
      "String(location.host ? document.cookie : '')",
      Some(move |r: Result<Value, Value>| {
        let _ = tx.send(r);
      }),
    );
    if let Ok(Ok(Ok(v))) =
      tokio::time::timeout(Duration::from_secs(1), rx).await
    {
      if let Some(s) = v.as_string() {
        if !s.is_empty() {
          return Some(s.to_string());
        }
      }
    }
    tokio::time::sleep(Duration::from_millis(200)).await;
  }
  None
}

fn cookie_encryption() -> String {
  let json = laufey::platform_features().unwrap_or_default();
  let key = "\"cookieEncryption\":\"";
  json
    .find(key)
    .map(|i| {
      let rest = &json[i + key.len()..];
      rest[..rest.find('"').unwrap_or(0)].to_string()
    })
    .unwrap_or_else(|| "none".to_string())
}

pub async fn run() {
  let phase = std::env::var("LAUFEY_E2E_KEYCHAIN_PHASE").unwrap_or_default();
  let expect = std::env::var("LAUFEY_E2E_KEYCHAIN_EXPECT").unwrap_or_default();
  let encryption = cookie_encryption();
  check(
    &format!(
      "keychain ({phase}): cookieEncryption is {expect:?} (got {encryption:?})"
    ),
    encryption == expect,
  );
  let Ok(listener) = TcpListener::bind("127.0.0.1:0") else {
    check("keychain: a loopback server for the page", false);
    return;
  };
  let port = listener.local_addr().map(|a| a.port()).unwrap_or(0);
  let set = phase == "set";
  std::thread::spawn(move || serve(listener, set));
  let win = Window::new(480, 320).load(&format!("http://127.0.0.1:{port}/"));
  let cookie = document_cookie(&win).await;
  check(
    &format!("keychain ({phase}): the page sees the cookie ({cookie:?})"),
    cookie.as_deref().is_some_and(|c| c.contains(COOKIE)),
  );
  // Give the cookie store a moment before the quit writes it out.
  let _ = wait_for(|| false, 10, 100).await;
  let _ = &win;
}
