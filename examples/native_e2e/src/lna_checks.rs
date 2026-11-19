// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Local Network Access (Chromium's private network protection; CEF only).
//! See docs/custom-schemes.md.
//!
//! - The app's own origin (`laufey-e2e://app`, a declared custom scheme)
//!   reaches its loopback servers: a WebSocket to the loopback echo server
//!   opens and receives a message (the cross-origin fetch is the main
//!   battery's Origin check). Every engine, since none may block it.
//! - Any other origin is still blocked: a page served from a loopback port
//!   that the CEF run declares public (`--ip-address-space-overrides`, set
//!   by native-e2e-run.sh) fetches the loopback echo server, and the fetch
//!   is refused at once, not left waiting for a prompt. CEF only.

use std::io::Write;
use std::net::TcpStream;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{Value, Window};

use super::{arg_int, arg_string, check, na, wait_for};

/// What the loopback WebSocket sends a client right after the handshake.
pub const WS_MESSAGE: &str = "ws-ok";

/// SHA-1 (RFC 3174), for the WebSocket handshake only.
fn sha1(data: &[u8]) -> [u8; 20] {
  let mut h: [u32; 5] =
    [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0];
  let mut msg = data.to_vec();
  let bit_len = (data.len() as u64).wrapping_mul(8);
  msg.push(0x80);
  while msg.len() % 64 != 56 {
    msg.push(0);
  }
  msg.extend_from_slice(&bit_len.to_be_bytes());
  for block in msg.chunks(64) {
    let mut w = [0u32; 80];
    for (i, word) in block.chunks(4).enumerate() {
      w[i] = u32::from_be_bytes([word[0], word[1], word[2], word[3]]);
    }
    for i in 16..80 {
      w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
    }
    let [mut a, mut b, mut c, mut d, mut e] = h;
    for (i, wi) in w.iter().enumerate() {
      let (f, k) = match i {
        0..=19 => ((b & c) | (!b & d), 0x5A827999),
        20..=39 => (b ^ c ^ d, 0x6ED9EBA1),
        40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1BBCDC),
        _ => (b ^ c ^ d, 0xCA62C1D6),
      };
      let t = a
        .rotate_left(5)
        .wrapping_add(f)
        .wrapping_add(e)
        .wrapping_add(k)
        .wrapping_add(*wi);
      e = d;
      d = c;
      c = b.rotate_left(30);
      b = a;
      a = t;
    }
    for (x, y) in h.iter_mut().zip([a, b, c, d, e]) {
      *x = x.wrapping_add(y);
    }
  }
  let mut out = [0u8; 20];
  for (i, x) in h.iter().enumerate() {
    out[i * 4..i * 4 + 4].copy_from_slice(&x.to_be_bytes());
  }
  out
}

fn base64(data: &[u8]) -> String {
  const T: &[u8] =
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  let mut out = String::new();
  for chunk in data.chunks(3) {
    let b = [
      chunk[0],
      *chunk.get(1).unwrap_or(&0),
      *chunk.get(2).unwrap_or(&0),
    ];
    let n = (b[0] as u32) << 16 | (b[1] as u32) << 8 | b[2] as u32;
    for i in 0..4 {
      if i <= chunk.len() {
        out.push(T[(n >> (18 - 6 * i) & 63) as usize] as char);
      } else {
        out.push('=');
      }
    }
  }
  out
}

/// The Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key.
fn ws_accept(key: &str) -> String {
  base64(&sha1(
    format!("{}258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key.trim()).as_bytes(),
  ))
}

/// How many WebSocket upgrade requests the loopback echo server got (for
/// the log when the page's socket reports no message).
static WS_UPGRADES: AtomicU32 = AtomicU32::new(0);

/// Answers a WebSocket upgrade request (`head` is its header block): the
/// handshake, one text frame with [`WS_MESSAGE`], then a close frame.
pub fn serve_ws(mut stream: TcpStream, head: &str) {
  WS_UPGRADES.fetch_add(1, Ordering::SeqCst);
  let Some(key) = head.lines().find_map(|l| {
    let (name, value) = l.split_once(':')?;
    name
      .trim()
      .eq_ignore_ascii_case("sec-websocket-key")
      .then(|| value.trim().to_string())
  }) else {
    let _ = stream.write_all(b"HTTP/1.1 400 Bad Request\r\n\r\n");
    return;
  };
  let response = format!(
    "HTTP/1.1 101 Switching Protocols\r\nupgrade: websocket\r\n\
     connection: Upgrade\r\nsec-websocket-accept: {}\r\n\r\n",
    ws_accept(&key)
  );
  let mut frame = vec![0x81, WS_MESSAGE.len() as u8];
  frame.extend_from_slice(WS_MESSAGE.as_bytes());
  let _ = stream.write_all(response.as_bytes());
  let _ = stream.write_all(&frame);
  // Give the page a moment to read the message before the close.
  std::thread::sleep(Duration::from_millis(500));
  let _ = stream.write_all(&[0x88, 0x00]);
}

/// How long the page's WebSocket probe may take before it reports
/// `pending` (ms), and how long the harness waits for its report (the
/// latter a little longer). The probe ends on the socket's own events; the
/// bound only stops a socket that never settles (a cold CEF start on a busy
/// Windows runner took more than the 8 s this once allowed).
pub const WS_PROBE_LIMIT_MS: u64 = 45_000;
pub const WS_PROBE_WAIT_POLLS: u32 = 500; // of 100 ms: 50 s

/// Script run in the custom-scheme window: open a WebSocket to `url` and
/// report through the `schemeProbe` binding what happened: `message:<data>`
/// when its message arrives, else, at its error or close or after
/// [`WS_PROBE_LIMIT_MS`], what it got to (`error`, `closed` or `pending`),
/// its readyState and the timeline of its events.
pub fn ws_probe_js(label: &str, url: &str) -> String {
  let limit = WS_PROBE_LIMIT_MS;
  format!(
    r#"(() => {{
  let done = false;
  let ws = null;
  const t0 = performance.now();
  const events = [];
  const note = (what) => events.push(what + '@' + Math.round(performance.now() - t0) + 'ms');
  const report = async (out, settled) => {{
    if (done) return;
    done = true;
    if (!settled) {{
      out += '; readyState ' + (ws ? ws.readyState : 'none') +
        '; events ' + (events.join(' ') || 'none');
    }}
    for (let i = 0; i < 100; i++) {{
      if (typeof Laufey !== 'undefined' &&
          typeof Laufey.schemeProbe === 'function') break;
      await new Promise(r => setTimeout(r, 50));
    }}
    Laufey.schemeProbe({label:?}, out);
  }};
  setTimeout(() => report('pending'), {limit});
  try {{
    ws = new WebSocket({url:?});
    note('created');
    ws.onopen = () => note('open');
    ws.onmessage = (e) => {{ note('message'); report('message:' + e.data, true); ws.close(); }};
    ws.onerror = () => {{ note('error'); report('error'); }};
    ws.onclose = (e) => {{ note('close ' + e.code); report('closed'); }};
  }} catch (e) {{
    report('threw: ' + (e && e.message));
  }}
}})();"#
  )
}

/// The page at `http://127.0.0.1:<public port>/`: fetches `echo_url` (a
/// loopback server) and reports through the `lnaProbe` binding how it went
/// (`served:<body>`, `rejected`, or `pending` after 8 s) and how long it
/// took.
fn public_page_html(echo_url: &str) -> String {
  format!(
    r#"<!doctype html><html><head><meta charset="utf-8"><title>public</title></head><body>
<script>
const t0 = performance.now();
let done = false;
async function report(out) {{
  if (done) return;
  done = true;
  for (let i = 0; i < 100; i++) {{
    if (typeof Laufey !== 'undefined' && typeof Laufey.lnaProbe === 'function') break;
    await new Promise(r => setTimeout(r, 50));
  }}
  await Laufey.lnaProbe(out, Math.round(performance.now() - t0),
                        location.origin + ' secure=' + window.isSecureContext);
}}
setTimeout(() => report('pending'), 8000);
fetch({echo_url:?}, {{ mode: 'cors' }})
  .then(r => r.text())
  .then(t => report('served:' + t), () => report('rejected'));
</script></body></html>"#
  )
}

/// Serves `html` to every request on `port` (loopback) from a thread per
/// connection; the bind error if the port can't be bound.
fn serve_page(port: u16, html: String) -> std::io::Result<()> {
  let listener = std::net::TcpListener::bind(("127.0.0.1", port))?;
  std::thread::spawn(move || {
    for stream in listener.incoming() {
      let Ok(mut stream) = stream else { continue };
      let html = html.clone();
      std::thread::spawn(move || {
        let _ = stream.set_read_timeout(Some(Duration::from_secs(5)));
        let mut buf = Vec::new();
        let mut chunk = [0u8; 4096];
        while !buf.windows(4).any(|w| w == b"\r\n\r\n") {
          match std::io::Read::read(&mut stream, &mut chunk) {
            Ok(0) | Err(_) => return,
            Ok(n) => buf.extend_from_slice(&chunk[..n]),
          }
        }
        let response = format!(
          "HTTP/1.1 200 OK\r\ncontent-type: text/html; charset=utf-8\r\n\
           cache-control: no-store\r\ncontent-length: {}\r\n\
           connection: close\r\n\r\n{}",
          html.len(),
          html
        );
        let _ = stream.write_all(response.as_bytes());
      });
    }
  });
  Ok(())
}

/// LAUFEY_E2E_ONLY=lna: the custom-scheme page (its cross-origin fetch and
/// WebSocket to the loopback echo server), then the other-origin check.
pub async fn run(echo_url: Option<&str>) {
  if !laufey::scheme_handlers_supported() {
    na("Local Network Access (backend has no web engine)");
    return;
  }
  let report: Arc<Mutex<Option<String>>> = Arc::new(Mutex::new(None));
  let probes: Arc<Mutex<std::collections::HashMap<String, String>>> =
    Arc::default();
  let w = {
    let (report, probes) = (report.clone(), probes.clone());
    Window::new(480, 360)
      .title("native-e2e-lna-app")
      .bind("schemeReport", move |call| {
        // The cross-origin fetch's result (see scheme_page_html).
        *report.lock().unwrap() = Some(arg_string(&call.args, 8));
        call.resolve(Value::Bool(true));
      })
      .bind("schemeProbe", move |call| {
        probes
          .lock()
          .unwrap()
          .insert(arg_string(&call.args, 0), arg_string(&call.args, 1));
        call.resolve(Value::Bool(true));
      })
      .load("laufey-e2e://app/")
  };
  w.show();
  let reported = wait_for(|| report.lock().unwrap().is_some(), 200, 100).await;
  let echo = report.lock().unwrap().clone().unwrap_or_default();
  let fetched = echo == "origin=laufey-e2e://app";
  if !fetched {
    log_loopback_connections(echo_url);
  }
  if let Some(reason) = loopback_skip_reason() {
    na(&format!(
      "a fetch from the custom-scheme page reaches a loopback server ({reason})"
    ));
  } else {
    check(
      &format!(
        "a fetch from the custom-scheme page reaches a loopback server \
       (reported {reported}, got {echo:?})"
      ),
      fetched,
    );
  }
  wait_for(
    || probes.lock().unwrap().contains_key("websocket"),
    WS_PROBE_WAIT_POLLS,
    100,
  )
  .await;
  let got = probes
    .lock()
    .unwrap()
    .get("websocket")
    .cloned()
    .unwrap_or_default();
  check_app_origin_websocket(echo_url, &got);
  other_origin_blocked(echo_url).await;
}

/// After a loopback request from the page didn't arrive: the echo server's
/// port and every TCP connection to or from it (state, owning process), so
/// the log shows whether the engine tried to connect at all. Windows only
/// (GetExtendedTcpTable); a no-op elsewhere.
pub fn log_loopback_connections(echo_url: Option<&str>) {
  let Some(port) = echo_url.and_then(|u| {
    u.split("://")
      .nth(1)?
      .split('/')
      .next()?
      .rsplit(':')
      .next()?
      .parse::<u16>()
      .ok()
  }) else {
    return;
  };
  #[cfg(windows)]
  {
    let rows = tcp_table::connections_on_port(port);
    eprintln!(
      "[e2e] loopback echo server port {port}: {} TCP row(s){}",
      rows.len(),
      if rows.is_empty() { "" } else { ":" }
    );
    for row in rows {
      eprintln!("[e2e]   {row}");
    }
  }
  #[cfg(not(windows))]
  eprintln!("[e2e] loopback echo server port {port}");
}

#[cfg(windows)]
mod tcp_table {
  use std::ffi::c_void;

  #[link(name = "iphlpapi")]
  extern "system" {
    fn GetExtendedTcpTable(
      table: *mut c_void,
      size: *mut u32,
      order: i32,
      af: u32,
      class: i32,
      reserved: u32,
    ) -> u32;
  }

  extern "system" {
    fn OpenProcess(access: u32, inherit: i32, pid: u32) -> *mut c_void;
    fn CloseHandle(h: *mut c_void) -> i32;
    fn QueryFullProcessImageNameW(
      process: *mut c_void,
      flags: u32,
      name: *mut u16,
      size: *mut u32,
    ) -> i32;
  }

  const AF_INET: u32 = 2;
  const TCP_TABLE_OWNER_PID_ALL: i32 = 5;
  const PROCESS_QUERY_LIMITED_INFORMATION: u32 = 0x1000;

  #[repr(C)]
  #[derive(Clone, Copy)]
  struct Row {
    state: u32,
    local_addr: u32,
    local_port: u32,
    remote_addr: u32,
    remote_port: u32,
    pid: u32,
  }

  fn state_name(state: u32) -> &'static str {
    match state {
      1 => "CLOSED",
      2 => "LISTEN",
      3 => "SYN_SENT",
      4 => "SYN_RCVD",
      5 => "ESTABLISHED",
      6 => "FIN_WAIT1",
      7 => "FIN_WAIT2",
      8 => "CLOSE_WAIT",
      9 => "CLOSING",
      10 => "LAST_ACK",
      11 => "TIME_WAIT",
      12 => "DELETE_TCB",
      _ => "?",
    }
  }

  fn addr(a: u32, port: u32) -> String {
    let b = a.to_le_bytes();
    let p = u16::from_be((port & 0xffff) as u16);
    format!("{}.{}.{}.{}:{p}", b[0], b[1], b[2], b[3])
  }

  fn process_name(pid: u32) -> String {
    let h = unsafe { OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid) };
    if h.is_null() {
      return "?".into();
    }
    let mut buf = [0u16; 260];
    let mut len = buf.len() as u32;
    let ok =
      unsafe { QueryFullProcessImageNameW(h, 0, buf.as_mut_ptr(), &mut len) };
    unsafe { CloseHandle(h) };
    if ok == 0 {
      return "?".into();
    }
    let path = String::from_utf16_lossy(&buf[..len as usize]);
    path.rsplit('\\').next().unwrap_or(&path).to_string()
  }

  pub fn connections_on_port(port: u16) -> Vec<String> {
    let mut size = 0u32;
    unsafe {
      GetExtendedTcpTable(
        std::ptr::null_mut(),
        &mut size,
        0,
        AF_INET,
        TCP_TABLE_OWNER_PID_ALL,
        0,
      )
    };
    if size == 0 {
      return Vec::new();
    }
    let mut buf = vec![0u32; (size as usize).div_ceil(4) + 16];
    size = (buf.len() * 4) as u32;
    if unsafe {
      GetExtendedTcpTable(
        buf.as_mut_ptr() as *mut c_void,
        &mut size,
        0,
        AF_INET,
        TCP_TABLE_OWNER_PID_ALL,
        0,
      )
    } != 0
    {
      return Vec::new();
    }
    let count = buf[0] as usize;
    let rows = unsafe {
      std::slice::from_raw_parts(buf.as_ptr().add(1) as *const Row, count)
    };
    rows
      .iter()
      .filter(|r| {
        let lp = u16::from_be((r.local_port & 0xffff) as u16);
        let rp = u16::from_be((r.remote_port & 0xffff) as u16);
        lp == port || rp == port
      })
      .map(|r| {
        format!(
          "{} {} -> {} pid {} ({})",
          state_name(r.state),
          addr(r.local_addr, r.local_port),
          addr(r.remote_addr, r.remote_port),
          r.pid,
          process_name(r.pid)
        )
      })
      .collect()
  }
}

/// The WebSocket URL on the loopback echo server at `echo_url`.
pub fn ws_url(echo_url: &str) -> String {
  echo_url
    .replacen("http://", "ws://", 1)
    .replacen("/echo", "/ws", 1)
}

/// The custom-scheme page's WebSocket to the loopback echo server opened
/// and got its message (`got` is what the page reported).
/// LAUFEY_E2E_LOOPBACK=0 (WebView2): the custom-scheme page's requests to
/// loopback are N/A, for a machine whose WebView2 holds them without ever
/// sending or failing them (docs/custom-schemes.md); the reason, else None.
pub fn loopback_skip_reason() -> Option<&'static str> {
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  (backend == "webview"
    && cfg!(windows)
    && std::env::var("LAUFEY_E2E_LOOPBACK").as_deref() == Ok("0"))
  .then_some(
    "LAUFEY_E2E_LOOPBACK=0: this machine's WebView2 holds a custom-scheme \
     page's loopback requests, never sending or failing them",
  )
}

pub fn check_app_origin_websocket(echo_url: Option<&str>, got: &str) {
  if let Some(reason) = loopback_skip_reason() {
    na(&format!("a WebSocket to loopback ({reason})"));
    return;
  }
  if echo_url.is_none() {
    na("a WebSocket to loopback (could not bind a loopback echo server)");
    return;
  }
  let ok = got == format!("message:{WS_MESSAGE}");
  if !ok {
    eprintln!(
      "[e2e] INFO the loopback echo server got {} WebSocket upgrade request(s)",
      WS_UPGRADES.load(Ordering::SeqCst)
    );
    log_loopback_connections(echo_url);
  }
  check(
    &format!(
      "a WebSocket from the custom-scheme page to a loopback server gets \
       its message (got {got:?})"
    ),
    ok,
  );
}

/// CEF: a page on an origin that is neither the app's nor loopback (a
/// loopback port this run declares public) can't reach loopback: its fetch
/// is refused, and at once (the prompt is answered, not left pending).
pub async fn other_origin_blocked(echo_url: Option<&str>) {
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  if backend != "cef" {
    na("Local Network Access for other origins (CEF only: Chromium's check)");
    return;
  }
  let Some(echo) = echo_url else {
    na("Local Network Access for other origins (no loopback echo server)");
    return;
  };
  // The ports the run declared public (native-e2e-run.sh): the page is
  // served on the first one that is free.
  let ports: Vec<u16> = std::env::var("LAUFEY_E2E_PUBLIC_PORT")
    .unwrap_or_default()
    .split(',')
    .filter_map(|p| p.trim().parse::<u16>().ok())
    .collect();
  if ports.is_empty() {
    na(
      "Local Network Access for other origins (LAUFEY_E2E_PUBLIC_PORT unset; \
       native-e2e-run.sh sets it)",
    );
    return;
  }
  let mut errors = Vec::new();
  let Some(port) = ports.iter().copied().find(|&p| {
    match serve_page(p, public_page_html(echo)) {
      Ok(()) => true,
      Err(e) => {
        errors.push(format!("{p}: {e}"));
        false
      }
    }
  }) else {
    check(
      &format!("the public page server binds a declared port ({errors:?})"),
      false,
    );
    return;
  };
  if !errors.is_empty() {
    eprintln!("[e2e] INFO public ports taken: {errors:?}");
  }
  let result: Arc<Mutex<Option<(String, i32, String)>>> =
    Arc::new(Mutex::new(None));
  let w = {
    let result = result.clone();
    Window::new(320, 240)
      .title("native-e2e-lna")
      .bind("lnaProbe", move |call| {
        let a = &call.args;
        *result.lock().unwrap() =
          Some((arg_string(a, 0), arg_int(a, 1), arg_string(a, 2)));
        call.resolve(Value::Bool(true));
      })
      .load(&format!("http://127.0.0.1:{port}/"))
  };
  w.show();
  let reported = wait_for(|| result.lock().unwrap().is_some(), 200, 100).await;
  let (out, ms, page) = result
    .lock()
    .unwrap()
    .clone()
    .unwrap_or_else(|| ("no report".into(), -1, String::new()));
  check(
    &format!(
      "a page on another origin can't reach loopback: its fetch is refused \
       at once (got {out:?} after {ms} ms, page {page})"
    ),
    reported && out == "rejected" && (0..5000).contains(&ms),
  );
  w.close();
}
