//! Incremental responses over a custom scheme.
//!
//! A page at `app://e2e-stream/` reads responses that never end while it
//! reads them (fetch, EventSource and XMLHttpRequest), aborts one through an
//! AbortController, and reads a binary body larger than the WebView2
//! backend's credit window. Each never-ending route writes a little, waits,
//! writes more, then keeps writing heartbeats until a write fails, so a page
//! that sees its first chunks proves they arrived before the response ended,
//! and the failed write proves the engine's cancellation reached the
//! handler. See docs/custom-schemes.md ("Streaming responses").

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use laufey::{SchemeRequest, Value, Window};

use crate::{arg_bool, arg_string, check, na, wait_for};

pub const PAGE_URL: &str = "app://e2e-stream/";
const PAGE_PREFIX: &str = "app://e2e-stream";

/// Length of the binary body: over the WebView2 credit window (4 MiB) and
/// not a multiple of any plausible chunk size.
const BIN_LEN: usize = 6 * 1024 * 1024 + 13;

/// The scenarios the page runs; each reports once.
const LABELS: [&str; 6] = ["fetch", "abort", "sse", "xhr", "bin", "fast"];
/// Labels whose never-ending route must see its write fail (the engine
/// cancelled the request) once the page is done with it.
const CANCELLED: [&str; 4] = ["fetch", "abort", "sse", "xhr"];

/// Shared between the scheme handler and the checks.
#[derive(Clone, Default)]
pub struct State {
  /// Never-ending routes whose write failed, by label.
  cancelled: Arc<Mutex<HashSet<String>>>,
}

fn pattern(len: usize) -> Vec<u8> {
  (0..len)
    .map(|i| ((i % 251) ^ ((i / 251) & 0xff)) as u8)
    .collect()
}

fn page_html() -> String {
  format!(
    r#"<!doctype html><html><head><meta charset="utf-8"><title>stream</title></head><body>
<script>
async function waitForBinding(name) {{
  for (let i = 0; i < 200; i++) {{
    if (typeof Laufey !== 'undefined' && typeof Laufey[name] === 'function') return;
    await new Promise(r => setTimeout(r, 50));
  }}
  throw new Error('binding never appeared: ' + name);
}}
const sleep = ms => new Promise(r => setTimeout(r, ms));
// Rejects after `ms` so a scenario that hangs still reports.
function deadline(p, ms, what) {{
  return Promise.race([p, sleep(ms).then(() => {{ throw new Error('timed out: ' + what); }})]);
}}
async function report(label, ok, detail) {{
  await waitForBinding('streamReport');
  await Laufey.streamReport(label, !!ok, String(detail));
}}
// Reads until `want` has arrived; resolves with the reader still open.
async function readUntil(reader, want) {{
  const dec = new TextDecoder();
  let text = '', reads = 0;
  while (!text.includes(want)) {{
    const {{ value, done }} = await reader.read();
    if (done) throw new Error('ended early after ' + JSON.stringify(text));
    reads++;
    text += dec.decode(value, {{ stream: true }});
  }}
  return {{ text, reads }};
}}
const scenarios = {{
  // A response that never ends delivers its first chunks, with its status
  // and headers, and cancelling the reader stops it.
  async fetch() {{
    const res = await fetch('/open?k=fetch');
    const reader = res.body.getReader();
    const got = await readUntil(reader, 'a|b|');
    await reader.cancel();
    const head = res.status === 201 && res.headers.get('x-e2e') === 'yes' &&
      !res.headers.has('x-laufey-stream');
    return [head, 'status ' + res.status + ' ' + res.statusText + ', x-e2e ' +
      res.headers.get('x-e2e') + ', ' + got.reads + ' read(s)'];
  }},
  // Aborting mid-body fails the pending read with an AbortError.
  async abort() {{
    const ac = new AbortController();
    const res = await fetch('/open?k=abort', {{ signal: ac.signal }});
    const reader = res.body.getReader();
    await readUntil(reader, 'a|');
    ac.abort();
    try {{
      for (;;) {{ const {{ done }} = await reader.read(); if (done) return [false, 'ended instead of aborting']; }}
    }} catch (e) {{
      return [e && e.name === 'AbortError', 'read rejected with ' + (e && e.name)];
    }}
  }},
  // EventSource: a default and a named event, the event id, then close().
  async sse() {{
    const es = new EventSource('/sse');
    const seen = [];
    await new Promise((resolve, reject) => {{
      es.onmessage = e => {{ seen.push('message:' + e.data); }};
      es.addEventListener('tick', e => {{ seen.push('tick:' + e.data + '#' + e.lastEventId); resolve(); }});
      es.onerror = () => {{ if (es.readyState === EventSource.CLOSED) reject(new Error('closed')); }};
    }});
    const open = es.readyState === EventSource.OPEN;
    es.close();
    const want = 'message:one,tick:two#7';
    return [open && seen.join(',') === want && es.readyState === EventSource.CLOSED,
            seen.join(',') + ' readyState ' + es.readyState];
  }},
  // XMLHttpRequest reaches LOADING with the first chunks in responseText.
  async xhr() {{
    const xhr = new XMLHttpRequest();
    const states = [];
    const text = await new Promise((resolve, reject) => {{
      xhr.onreadystatechange = () => {{
        states.push(xhr.readyState);
        if (xhr.readyState === 3 && xhr.responseText.includes('a|b|')) resolve(xhr.responseText);
      }};
      xhr.onerror = () => reject(new Error('xhr error'));
      xhr.open('GET', '/open?k=xhr');
      xhr.send();
    }});
    const status = xhr.status;
    xhr.abort();
    return [status === 201 && text.startsWith('a|b|'),
            'status ' + status + ', states ' + states.join(',')];
  }},
  // A large binary body arrives intact, byte for byte.
  async bin() {{
    const res = await fetch('/bin');
    const got = new Uint8Array(await res.arrayBuffer());
    const n = {bin_len};
    let bad = got.length === n ? -1 : 0;
    for (let i = 0; bad < 0 && i < n; i++) {{
      if (got[i] !== ((i % 251) ^ (Math.floor(i / 251) & 255))) bad = i;
    }}
    return [bad < 0, 'length ' + got.length + (bad >= 0 ? ', first difference at ' + bad : '')];
  }},
  // A response that is complete at once is unaffected.
  async fast() {{
    const res = await fetch('/fast');
    const text = await res.text();
    return [res.status === 200 && text === 'fast body', 'status ' + res.status + ' ' + JSON.stringify(text)];
  }},
}};
(async () => {{
  for (const [label, run] of Object.entries(scenarios)) {{
    try {{
      const [ok, detail] = await deadline(run(), 15000, label);
      await report(label, ok, detail);
    }} catch (e) {{
      await report(label, false, 'error: ' + (e && e.message));
    }}
  }}
}})().catch(e => report('script', false, String(e && e.message)));
</script></body></html>"#,
    bin_len = BIN_LEN,
  )
}

fn begin(req: &SchemeRequest, status: i32, content_type: &str) {
  let mut headers = vec![
    ("content-type".to_string(), content_type.to_string()),
    ("cache-control".to_string(), "no-store".to_string()),
  ];
  if status == 201 {
    headers.push(("x-e2e".to_string(), "yes".to_string()));
  }
  req.exchange.begin(status, &headers);
}

/// Writes `first`, then `second` after a pause, then heartbeats until a write
/// fails (recorded under `label`) or a minute passes.
fn never_ending(
  req: SchemeRequest,
  state: State,
  label: String,
  first: &str,
  second: &str,
  heartbeat: &str,
) {
  let mut failed = req.exchange.write(first.as_bytes()) < 0;
  if !failed {
    std::thread::sleep(Duration::from_millis(300));
    failed = req.exchange.write(second.as_bytes()) < 0;
  }
  let start = Instant::now();
  while !failed && start.elapsed() < Duration::from_secs(60) {
    std::thread::sleep(Duration::from_millis(200));
    failed = req.exchange.write(heartbeat.as_bytes()) < 0;
  }
  if failed {
    state.cancelled.lock().unwrap().insert(label);
  }
  req.exchange.finish();
}

/// Serve `req` if it belongs to these checks; otherwise hand it back. Every
/// route runs on its own thread: the handler must not block the backend
/// thread it is called on.
pub fn serve(req: SchemeRequest, state: &State) -> Option<SchemeRequest> {
  let url = req.url.clone();
  let path = url.split(['?', '#']).next().unwrap_or("").to_string();
  if path == PAGE_PREFIX || path == PAGE_URL {
    begin(&req, 200, "text/html");
    req.exchange.write(page_html().as_bytes());
    req.exchange.finish();
    return None;
  }
  let Some(route) = path.strip_prefix("app://e2e-stream/") else {
    return Some(req);
  };
  let route = route.to_string();
  let label = url
    .split_once("k=")
    .map(|(_, k)| k.to_string())
    .unwrap_or_default();
  let state = state.clone();
  std::thread::spawn(move || match route.as_str() {
    "open" => {
      begin(&req, 201, "text/plain; charset=utf-8");
      never_ending(req, state, label, "a|", "b|", ".");
    }
    "sse" => {
      begin(&req, 200, "text/event-stream");
      never_ending(
        req,
        state,
        "sse".to_string(),
        "retry: 60000\ndata: one\n\n",
        "event: tick\nid: 7\ndata: two\n\n",
        ":hb\n\n",
      );
    }
    "bin" => {
      begin(&req, 200, "application/octet-stream");
      // Late enough that a buffering backend streams it instead.
      std::thread::sleep(Duration::from_millis(150));
      let body = pattern(BIN_LEN);
      for chunk in body.chunks(256 * 1024) {
        if req.exchange.write(chunk) < 0 {
          break;
        }
      }
      req.exchange.finish();
    }
    "fast" => {
      begin(&req, 200, "text/plain");
      req.exchange.write(b"fast body");
      req.exchange.finish();
    }
    _ => {
      begin(&req, 404, "text/plain");
      req.exchange.finish();
    }
  });
  None
}

/// Opens the page and checks what it and the handler observed. Returns the
/// window so the caller decides when it closes.
pub async fn run(state: &State) -> Option<Window> {
  if !laufey::scheme_handlers_supported() {
    na("incremental custom-scheme responses (backend has no scheme handler support)");
    return None;
  }
  let reports: Arc<Mutex<HashMap<String, (bool, String)>>> =
    Arc::new(Mutex::new(HashMap::new()));
  let win = Window::new(320, 240)
    .title("native-e2e-stream")
    .bind("streamReport", {
      let reports = reports.clone();
      move |call| {
        let a = &call.args;
        reports
          .lock()
          .unwrap()
          .insert(arg_string(a, 0), (arg_bool(a, 1), arg_string(a, 2)));
        call.resolve(Value::Bool(true));
      }
    })
    .load(PAGE_URL);

  let done = wait_for(
    || {
      let r = reports.lock().unwrap();
      r.contains_key("script") || LABELS.iter().all(|l| r.contains_key(*l))
    },
    900,
    100,
  )
  .await;
  check("streaming page reported every scenario", done);
  let got = reports.lock().unwrap().clone();
  if let Some((_, detail)) = got.get("script") {
    check(&format!("streaming page script ran ({detail})"), false);
  }
  let describe = |label: &str| {
    match label {
    "fetch" => "fetch reads a never-ending response incrementally, with its status and headers",
    "abort" => "AbortController aborts a streaming fetch mid-body",
    "sse" => "EventSource receives events from a never-ending response, then closes",
    "xhr" => "XMLHttpRequest reaches LOADING with the first chunks of a never-ending response",
    "bin" => "a large binary body arrives intact",
    _ => "a response complete at once is unaffected",
  }
  };
  for label in LABELS {
    match got.get(label) {
      Some((ok, detail)) => {
        check(&format!("{} ({detail})", describe(label)), *ok)
      }
      None => check(&format!("{} (no report)", describe(label)), false),
    }
  }
  // The engine's cancellation must reach the handler: its next write fails.
  let all_cancelled = wait_for(
    || {
      let c = state.cancelled.lock().unwrap();
      CANCELLED.iter().all(|l| c.contains(*l))
    },
    50,
    100,
  )
  .await;
  let cancelled = state.cancelled.lock().unwrap().clone();
  let mut missing: Vec<&str> = CANCELLED
    .iter()
    .copied()
    .filter(|l| !cancelled.contains(*l))
    .collect();
  missing.sort();
  check(
    &format!(
      "cancelling a never-ending response stops the handler's writes (still writing: {missing:?})"
    ),
    all_cancelled,
  );
  Some(win)
}
