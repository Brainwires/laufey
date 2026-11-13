//! Child web views (API 58): part of the window-api battery.
//!
//! A loopback server serves two pages; a web view in the battery's window
//! loads them and must report its load / title / URL events, follow back /
//! forward, report its state, evaluate in an isolated world without the
//! page's bridge, capture, refuse non-http(s) URLs, hold at most
//! LAUFEY_WEBVIEW_MAX_PER_WINDOW views per window, and fire CLOSED once.
//! A backend without web views (Winit) must say so and refuse to create one.

use std::io::{BufRead, BufReader, Write};
use std::net::TcpListener;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{
  CaptureFormat, WebViewBounds, WebViewError, WebViewEvent, WebViewEventKind,
  WebViewOptions, Window,
};

use crate::{check, na};

const PAGE_A: &str = "<!doctype html><title>webview-a</title>\
<script>console.warn('webview-a says hi')</script>\
<a id=next href=\"/b\">next</a>";
const PAGE_B: &str = "<!doctype html><title>webview-b</title><p>b</p>";

fn serve() -> Option<String> {
  let listener = TcpListener::bind("127.0.0.1:0").ok()?;
  let port = listener.local_addr().ok()?.port();
  std::thread::spawn(move || {
    for stream in listener.incoming().flatten() {
      std::thread::spawn(move || {
        let mut reader = BufReader::new(match stream.try_clone() {
          Ok(s) => s,
          Err(_) => return,
        });
        let mut line = String::new();
        if reader.read_line(&mut line).is_err() {
          return;
        }
        loop {
          let mut h = String::new();
          match reader.read_line(&mut h) {
            Ok(0) | Err(_) => break,
            Ok(_) if h == "\r\n" || h == "\n" => break,
            Ok(_) => {}
          }
        }
        let path = line.split_whitespace().nth(1).unwrap_or("/");
        let body = if path.starts_with("/b") {
          PAGE_B
        } else {
          PAGE_A
        };
        let response = format!(
          "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{body}",
          body.len()
        );
        let mut out = stream;
        let _ = out.write_all(response.as_bytes());
      });
    }
  });
  Some(format!("http://127.0.0.1:{port}"))
}

type Events = Arc<Mutex<Vec<WebViewEvent>>>;

fn recorder() -> (Events, impl Fn(WebViewEvent) + Send + Sync + 'static) {
  let events: Events = Arc::new(Mutex::new(Vec::new()));
  let sink = events.clone();
  (events, move |e| sink.lock().unwrap().push(e))
}

/// Waits (up to `secs`) for an event of `kind` whose detail contains
/// `needle`.
async fn saw(
  events: &Events,
  kind: WebViewEventKind,
  needle: &str,
  secs: u64,
) -> bool {
  for _ in 0..secs * 20 {
    if events
      .lock()
      .unwrap()
      .iter()
      .any(|e| e.kind == kind && e.detail_json.contains(needle))
    {
      return true;
    }
    tokio::time::sleep(Duration::from_millis(50)).await;
  }
  false
}

fn count(events: &Events, kind: WebViewEventKind) -> usize {
  events
    .lock()
    .unwrap()
    .iter()
    .filter(|e| e.kind == kind)
    .count()
}

async fn evaluate(
  view: laufey::WebView,
  script: &str,
) -> Result<String, laufey::WebViewFailure> {
  let (tx, rx) = tokio::sync::oneshot::channel();
  view.evaluate(script, move |r| {
    let _ = tx.send(r);
  });
  match tokio::time::timeout(Duration::from_secs(20), rx).await {
    Ok(Ok(r)) => r,
    _ => Err(laufey::WebViewFailure {
      code: "timeout",
      message: "no answer".into(),
    }),
  }
}

fn bounds(x: f64, y: f64, w: f64, h: f64) -> WebViewBounds {
  WebViewBounds {
    x,
    y,
    width: w,
    height: h,
  }
}

pub async fn run(w: &Window) {
  let caps = laufey::webview_capabilities();
  eprintln!("[e2e] webview capabilities = {:#x}", caps.0);
  if !caps.supported() {
    na("child web views (not supported by this backend)");
    let refused = laufey::create_webview(
      w.id(),
      &WebViewOptions {
        url: "https://example.com/".into(),
        ..Default::default()
      },
      |_| {},
    );
    check(
      "create_webview is refused as unsupported where not reported",
      refused == Err(WebViewError::Unsupported),
    );
    return;
  }
  check("web views name their engine", caps.engine().is_some());
  let Some(base) = serve() else {
    check("web view checks: a loopback server", false);
    return;
  };

  // Refusals before anything loads.
  let bad = |url: &str| WebViewOptions {
    url: url.into(),
    bounds: bounds(0.0, 0.0, 10.0, 10.0),
    ..Default::default()
  };
  for url in [
    "file:///etc/hosts",
    "data:text/html,x",
    "javascript:1",
    "about:blank",
  ] {
    check(
      &format!("create_webview refuses {url}"),
      laufey::create_webview(w.id(), &bad(url), |_| {})
        == Err(WebViewError::NotAllowedUrl),
    );
  }
  check(
    "create_webview refuses a bad partition as invalid",
    laufey::create_webview(
      w.id(),
      &WebViewOptions {
        partition: Some("../app".into()),
        ..bad("https://example.com/")
      },
      |_| {},
    ) == Err(WebViewError::Invalid),
  );
  check(
    "create_webview refuses an unknown window",
    laufey::create_webview(999_999, &bad("https://example.com/"), |_| {})
      == Err(WebViewError::NoWindow),
  );

  // A view that loads page A, then B.
  let (events, sink) = recorder();
  let created = laufey::create_webview(
    w.id(),
    &WebViewOptions {
      url: format!("{base}/a"),
      bounds: bounds(20.0, 30.0, 300.0, 200.0),
      partition: Some("ephemeral:e2e".into()),
      ..Default::default()
    },
    sink,
  );
  let Ok(view) = created else {
    check(&format!("create_webview succeeds ({created:?})"), false);
    return;
  };
  check(
    "a web view reports loading, then complete",
    saw(&events, WebViewEventKind::Load, "\"loading\"", 20).await
      && saw(&events, WebViewEventKind::Load, "\"complete\"", 20).await,
  );
  check(
    "a web view reports its page's title",
    saw(&events, WebViewEventKind::Title, "webview-a", 20).await,
  );
  check(
    "a web view reports its URL (no history yet)",
    saw(&events, WebViewEventKind::Url, "\"canGoBack\":false", 20).await,
  );
  check(
    "a web view reports its page's console",
    saw(&events, WebViewEventKind::Console, "webview-a says hi", 20).await,
  );
  let state = view.state().unwrap_or_default();
  check(
    "webview_state reports url, title, bounds and visibility",
    state.contains(&format!("\"url\":\"{base}/a\""))
      && state.contains("\"title\":\"webview-a\"")
      && state.contains(
        "\"bounds\":{\"x\":20,\"y\":30,\"width\":300,\"height\":200}",
      )
      && state.contains("\"visible\":true"),
  );

  check(
    "navigate to page B",
    view.navigate(&format!("{base}/b")).is_ok(),
  );
  check(
    "after a navigation the URL event can go back",
    saw(&events, WebViewEventKind::Url, "\"canGoBack\":true", 20).await
      && saw(&events, WebViewEventKind::Title, "webview-b", 20).await,
  );
  check("back()", view.back());
  check(
    "back() returns to page A",
    saw(&events, WebViewEventKind::Url, "\"canGoForward\":true", 20).await,
  );
  check(
    "navigate refuses non-http(s)",
    view.navigate("file:///etc/hosts") == Err(WebViewError::NotAllowedUrl)
      && view.navigate("javascript:alert(1)")
        == Err(WebViewError::NotAllowedUrl),
  );

  check(
    "set_bounds",
    view.set_bounds(bounds(10.0, 12.0, 240.0, 160.0)),
  );
  check("set_visible(false)", view.set_visible(false));
  let state = view.state().unwrap_or_default();
  check(
    "state follows set_bounds / set_visible",
    state
      .contains("\"bounds\":{\"x\":10,\"y\":12,\"width\":240,\"height\":160}")
      && state.contains("\"visible\":false"),
  );
  check(
    "set_bounds refuses a negative size",
    !view.set_bounds(bounds(0.0, 0.0, -1.0, 10.0)),
  );
  view.set_visible(true);

  if caps.evaluate() {
    tokio::time::sleep(Duration::from_millis(500)).await;
    let title = evaluate(view, "document.title").await;
    check(
      &format!("evaluate reads the DOM ({title:?})"),
      title.as_deref() == Ok("\"webview-a\""),
    );
    check(
      "evaluate returns JSON",
      evaluate(view, "1 + 1").await.as_deref() == Ok("2")
        && evaluate(view, "Promise.resolve({ a: [1, 'x'] })")
          .await
          .as_deref()
          == Ok("{\"a\":[1,\"x\"]}")
        && evaluate(view, "undefined").await.as_deref() == Ok("null"),
    );
    check(
      "evaluate reports a throw",
      evaluate(view, "throw new Error('boom')")
        .await
        .is_err_and(|f| f.code == "error" && f.message.contains("boom")),
    );
    check(
      "evaluate refuses a function as not serializable",
      evaluate(view, "() => 1")
        .await
        .is_err_and(|f| f.code == "not_serializable"),
    );
    // The page's own window bridge never reaches a web view.
    check(
      "a web view's page has no bridge",
      evaluate(
        view,
        "typeof globalThis.Laufey + ':' + typeof globalThis.Deno",
      )
      .await
      .as_deref()
        == Ok("\"undefined:undefined\""),
    );
  } else {
    na("evaluate (not supported by this backend)");
  }

  if caps.capture() {
    let (tx, rx) = tokio::sync::oneshot::channel();
    view.capture(CaptureFormat::Png, 0, None, move |r| {
      let _ = tx.send(r);
    });
    let png = tokio::time::timeout(Duration::from_secs(20), rx).await;
    let ok = matches!(&png, Ok(Ok(Ok(bytes))) if bytes.starts_with(b"\x89PNG"));
    check("capture returns a PNG", ok);
    // Timing for the snapshot pattern (an <img> swapped in while HTML
    // covers the view): reported, not gated, as runners vary.
    for (label, format, scale) in [
      ("png device", CaptureFormat::Png, None),
      ("jpeg device", CaptureFormat::Jpeg, None),
      ("jpeg 1x", CaptureFormat::Jpeg, Some(1.0)),
      ("png 1x", CaptureFormat::Png, Some(1.0)),
    ] {
      let started = std::time::Instant::now();
      let (tx, rx) = tokio::sync::oneshot::channel();
      view.capture(format, 80, scale, move |r| {
        let _ = tx.send(r.map(|b| b.len()));
      });
      let got = tokio::time::timeout(Duration::from_secs(20), rx).await;
      eprintln!(
        "[e2e] INFO capture {label}: {:?} in {} ms",
        got.ok().and_then(|r| r.ok()),
        started.elapsed().as_millis()
      );
    }
  } else {
    na("capture (not supported by this backend)");
  }

  // Stacking among the window's views.
  {
    let (_, sink) = recorder();
    if let Ok(top) = laufey::create_webview(
      w.id(),
      &WebViewOptions {
        url: format!("{base}/b"),
        bounds: bounds(40.0, 40.0, 100.0, 100.0),
        partition: Some("ephemeral:e2e".into()),
        ..Default::default()
      },
      sink,
    ) {
      let z = |v: laufey::WebView| {
        v.state()
          .and_then(|s| {
            s.split("\"zOrder\":")
              .nth(1)
              .map(|t| t.trim_end_matches('}').to_string())
          })
          .and_then(|t| t.parse::<i32>().ok())
      };
      check(
        "a new web view starts above the window's others",
        z(top).unwrap_or(i32::MIN) > z(view).unwrap_or(i32::MAX),
      );
      if caps.z_order() {
        check("raise()", view.raise());
        check(
          "raise() puts a view above the others",
          z(view).unwrap_or(i32::MIN) > z(top).unwrap_or(i32::MAX),
        );
        check("lower()", view.lower());
        check(
          "lower() puts it below",
          z(view).unwrap_or(i32::MAX) < z(top).unwrap_or(i32::MIN),
        );
        check("set_z_order(7)", view.set_z_order(7) && z(view) == Some(7));
      } else {
        check(
          "stacking is refused where not reported",
          !view.raise() && !view.lower() && !view.set_z_order(1),
        );
      }
      top.close();
    }
  }

  // The per-window limit.
  let mut extra = Vec::new();
  let mut limited = false;
  for _ in 0..40 {
    match laufey::create_webview(
      w.id(),
      &WebViewOptions {
        url: format!("{base}/b"),
        bounds: bounds(0.0, 0.0, 1.0, 1.0),
        visible: Some(false),
        partition: Some("ephemeral:limit".into()),
        ..Default::default()
      },
      |_| {},
    ) {
      Ok(v) => extra.push(v),
      Err(WebViewError::Limit) => {
        limited = true;
        break;
      }
      Err(_) => break,
    }
  }
  check(
    "a window holds at most LAUFEY_WEBVIEW_MAX_PER_WINDOW web views",
    limited && extra.len() + 1 == 32,
  );
  for v in extra {
    v.close();
  }

  // Close: CLOSED exactly once, then every call is refused.
  check("close()", view.close());
  check(
    "CLOSED fires",
    saw(&events, WebViewEventKind::Closed, "{}", 10).await,
  );
  tokio::time::sleep(Duration::from_millis(300)).await;
  check(
    "CLOSED fires exactly once and last",
    count(&events, WebViewEventKind::Closed) == 1
      && events.lock().unwrap().last().map(|e| e.kind)
        == Some(WebViewEventKind::Closed),
  );
  check(
    "a closed web view refuses calls",
    !view.close()
      && view.state().is_none()
      && view.navigate(&format!("{base}/a")) == Err(WebViewError::Closed)
      && !view.set_visible(true),
  );

  // A window's views close with it.
  let w2 = Window::new(320, 240).title("native-e2e-webview-owner");
  w2.show();
  tokio::time::sleep(Duration::from_millis(500)).await;
  let (owned_events, owned_sink) = recorder();
  if let Ok(_owned) = laufey::create_webview(
    w2.id(),
    &WebViewOptions {
      url: format!("{base}/b"),
      bounds: bounds(0.0, 0.0, 100.0, 100.0),
      ..Default::default()
    },
    owned_sink,
  ) {
    tokio::time::sleep(Duration::from_millis(500)).await;
    w2.close();
    check(
      "closing a window closes its web views",
      saw(&owned_events, WebViewEventKind::Closed, "{}", 15).await,
    );
  } else {
    check("a second window's web view", false);
    w2.close();
  }
}
