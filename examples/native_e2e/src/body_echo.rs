//! Request-body round trip over the custom scheme.
//!
//! A page served at `app://e2e-body/` sends POST/PUT/PATCH requests with
//! bodies (UTF-8 text, a small binary body with NUL and high bytes, a
//! binary body over 1 MB, and an empty body) to `app://e2e-body/echo/<label>`.
//! The scheme handler reads each body through `SchemeExchange::read_body`,
//! records what it received, and echoes it back byte for byte; the page
//! compares the echo with what it sent and reports through the `bodyReport`
//! binding. The runtime then checks both halves: the bytes the handler
//! received equal the expected bytes (regenerated here), and the page saw an
//! identical echo.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use laufey::SchemeRequest;

/// Page that drives the round trips.
pub const PAGE_URL: &str = "app://e2e-body/";
const PAGE_PREFIX: &str = "app://e2e-body";
const ECHO_PREFIX: &str = "app://e2e-body/echo/";

/// Size of the large binary body: over 1 MiB and not a multiple of any
/// plausible read chunk.
pub const BIG_LEN: usize = 1_536_007;

/// One request the page sends: label, HTTP method, expected body.
pub struct Case {
  pub label: &'static str,
  pub method: &'static str,
  pub body: Vec<u8>,
}

/// Deterministic binary pattern covering every byte value (0..=255,
/// including NUL and bytes >= 0x80). Mirrored by `pattern()` in the page.
fn pattern(len: usize) -> Vec<u8> {
  (0..len)
    .map(|i| ((i % 251) ^ ((i / 251) & 0xff)) as u8)
    .collect()
}

pub const TEXT_BODY: &str = "h\u{e9}llo, body \u{2713}\nline 2";

pub fn cases() -> Vec<Case> {
  vec![
    Case {
      label: "text",
      method: "POST",
      body: TEXT_BODY.as_bytes().to_vec(),
    },
    Case {
      label: "binary-small",
      method: "PATCH",
      body: vec![0, 1, 2, 0x7f, 0x80, 0xff, 0, 0xfe],
    },
    Case {
      label: "binary-big",
      method: "PUT",
      body: pattern(BIG_LEN),
    },
    Case {
      label: "empty",
      method: "POST",
      body: Vec::new(),
    },
  ]
}

/// What the handler received for one label: (method, body).
pub type Received = Arc<Mutex<HashMap<String, (String, Vec<u8>)>>>;

/// What the page reported for one label: (echo identical, echo length,
/// status/error detail).
pub type Reports = Arc<Mutex<HashMap<String, (bool, i64, String)>>>;

pub fn page_html() -> String {
  format!(
    r#"<!doctype html><html><head><meta charset="utf-8"><title>body</title></head><body>
<script>
async function waitForBinding(name) {{
  for (let i = 0; i < 200; i++) {{
    if (typeof Laufey !== 'undefined' && typeof Laufey[name] === 'function') return;
    await new Promise(r => setTimeout(r, 50));
  }}
  throw new Error('binding never appeared: ' + name);
}}
function pattern(n) {{
  const a = new Uint8Array(n);
  for (let i = 0; i < n; i++) a[i] = (i % 251) ^ (Math.floor(i / 251) & 255);
  return a;
}}
const cases = [
  ['text', 'POST', new TextEncoder().encode({text:?}), {text:?}],
  ['binary-small', 'PATCH', new Uint8Array([0, 1, 2, 0x7f, 0x80, 0xff, 0, 0xfe]), null],
  ['binary-big', 'PUT', pattern({big}), null],
  ['empty', 'POST', new Uint8Array(0), undefined],
];
(async () => {{
  await waitForBinding('bodyReport');
  for (const [label, method, bytes, sendAs] of cases) {{
    let same = false, len = -1, detail = '';
    try {{
      // `sendAs` sends the text case as a string (the engine encodes it) and
      // the empty case with no body at all; the rest as raw bytes.
      const body = sendAs === undefined ? undefined : (sendAs !== null ? sendAs : bytes);
      const res = await fetch('/echo/' + label, {{ method, body }});
      const got = new Uint8Array(await res.arrayBuffer());
      len = got.length;
      same = got.length === bytes.length && got.every((b, i) => b === bytes[i]);
      detail = 'status ' + res.status;
    }} catch (e) {{
      detail = 'error: ' + (e && e.message);
    }}
    await Laufey.bodyReport(label, same, len, detail);
  }}
}})().catch(e => Laufey.bodyReport('script', false, -1, String(e && e.message)));
</script></body></html>"#,
    text = TEXT_BODY,
    big = BIG_LEN,
  )
}

/// Serve `req` if it belongs to the body round trip; otherwise hand it back.
/// Echo requests are answered on their own thread: `read_body` may block, and
/// the scheme handler must not block the backend thread it runs on.
pub fn serve(req: SchemeRequest, received: &Received) -> Option<SchemeRequest> {
  let path = req.url.split(['?', '#']).next().unwrap_or("").to_string();
  if path == PAGE_PREFIX || path == PAGE_URL {
    let headers = vec![
      (
        "content-type".to_string(),
        "text/html; charset=utf-8".to_string(),
      ),
      ("cache-control".to_string(), "no-store".to_string()),
    ];
    req.exchange.begin(200, &headers);
    req.exchange.write(page_html().as_bytes());
    req.exchange.finish();
    return None;
  }
  let Some(label) = path.strip_prefix(ECHO_PREFIX).map(str::to_string) else {
    return Some(req);
  };
  let received = received.clone();
  std::thread::spawn(move || {
    let mut body = Vec::new();
    let mut buf = vec![0u8; 64 * 1024];
    let mut ok = true;
    loop {
      let n = req.exchange.read_body(&mut buf);
      if n == 0 {
        break;
      }
      if n < 0 {
        ok = false;
        break;
      }
      body.extend_from_slice(&buf[..n as usize]);
    }
    received
      .lock()
      .unwrap()
      .insert(label, (req.method.clone(), body.clone()));
    let headers = vec![
      (
        "content-type".to_string(),
        "application/octet-stream".to_string(),
      ),
      ("cache-control".to_string(), "no-store".to_string()),
    ];
    req.exchange.begin(if ok { 200 } else { 500 }, &headers);
    if !body.is_empty() {
      req.exchange.write(&body);
    }
    req.exchange.finish();
  });
  None
}
