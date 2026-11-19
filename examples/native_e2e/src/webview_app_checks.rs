// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Web view downloads and permission requests the app decides (API 59):
//! part of the window-api battery.
//!
//! A loopback server serves a page, a small attachment and a slow one. A web
//! view created with `downloads: "app"` must announce each download
//! (`"requested"`) and write nothing until the app answers: SAVE writes
//! exactly the chosen path (replacing a file there), CANCEL leaves nothing,
//! a download cancelled part-way or by closing the view ends with
//! `"cancelled"` (before CLOSED) and no partial file, and a finished or
//! unknown id is refused. Where reported, PAUSE / RESUME hold and release
//! the bytes. A view created with `permissions: "app"` must hand each
//! permission request to the app, refuse it when the app says so, and report
//! one that goes away unanswered as `"cancelled"`. An engine without the
//! capability must refuse the option rather than fall back to its default.

use std::io::{BufRead, BufReader, Write};
use std::net::TcpListener;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{
  DownloadAction, WebView, WebViewBounds, WebViewEngine, WebViewError,
  WebViewEvent, WebViewEventKind, WebViewOptions, Window,
};

use crate::{check, na};

const PAGE: &str = "<!doctype html><title>webview-app</title><p>app</p>";
const SMALL: &str = "laufey-dl!\n";
/// The slow attachment: 64 chunks of 64 KiB, one every 100 ms.
const SLOW_CHUNK: usize = 64 * 1024;
const SLOW_CHUNKS: usize = 64;

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
        let path = line.split_whitespace().nth(1).unwrap_or("/").to_string();
        let mut out = stream;
        if path.starts_with("/dl/small") {
          let _ = out.write_all(
            format!(
              "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\
Content-Disposition: attachment; filename=\"report.txt\"\r\n\
Content-Length: {}\r\nConnection: close\r\n\r\n{SMALL}",
              SMALL.len()
            )
            .as_bytes(),
          );
        } else if path.starts_with("/dl/slow") {
          let _ = out.write_all(
            format!(
              "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n\
Content-Disposition: attachment; filename=\"slow.bin\"\r\n\
Content-Length: {}\r\nConnection: close\r\n\r\n",
              SLOW_CHUNK * SLOW_CHUNKS
            )
            .as_bytes(),
          );
          let chunk = vec![b'x'; SLOW_CHUNK];
          for _ in 0..SLOW_CHUNKS {
            if out.write_all(&chunk).is_err() || out.flush().is_err() {
              return;
            }
            std::thread::sleep(Duration::from_millis(100));
          }
        } else {
          let _ = out.write_all(
            format!(
              "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\
Content-Length: {}\r\nConnection: close\r\n\r\n{PAGE}",
              PAGE.len()
            )
            .as_bytes(),
          );
        }
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

fn bounds() -> WebViewBounds {
  WebViewBounds {
    x: 0.0,
    y: 0.0,
    width: 200.0,
    height: 120.0,
  }
}

/// The events of `kind` so far, parsed, in order.
fn details(events: &Events, kind: WebViewEventKind) -> Vec<serde_json::Value> {
  events
    .lock()
    .unwrap()
    .iter()
    .filter(|e| e.kind == kind)
    .filter_map(|e| serde_json::from_str(&e.detail_json).ok())
    .collect()
}

/// Waits (up to `secs`) for an event of `kind` that passes `pred`.
async fn wait_event(
  events: &Events,
  kind: WebViewEventKind,
  secs: u64,
  pred: impl Fn(&serde_json::Value) -> bool,
) -> Option<serde_json::Value> {
  for _ in 0..secs * 20 {
    if let Some(found) = details(events, kind).into_iter().find(|d| pred(d)) {
      return Some(found);
    }
    tokio::time::sleep(Duration::from_millis(50)).await;
  }
  None
}

fn id_of(detail: &serde_json::Value) -> u32 {
  detail["id"].as_u64().unwrap_or(0) as u32
}

fn state_is(detail: &serde_json::Value, state: &str) -> bool {
  detail["state"].as_str() == Some(state)
}

/// The states of download `id`, in the order its events came.
fn states_of(events: &Events, id: u32) -> Vec<String> {
  details(events, WebViewEventKind::Download)
    .into_iter()
    .filter(|d| id_of(d) == id)
    .filter_map(|d| d["state"].as_str().map(str::to_string))
    .collect()
}

async fn gone(path: &Path, secs: u64) -> bool {
  for _ in 0..secs * 20 {
    if !path.exists() {
      return true;
    }
    tokio::time::sleep(Duration::from_millis(50)).await;
  }
  false
}

async fn evaluate(
  view: WebView,
  script: &str,
  secs: u64,
) -> Result<String, laufey::WebViewFailure> {
  let (tx, rx) = tokio::sync::oneshot::channel();
  view.evaluate(script, move |r| {
    let _ = tx.send(r);
  });
  match tokio::time::timeout(Duration::from_secs(secs), rx).await {
    Ok(Ok(r)) => r,
    _ => Err(laufey::WebViewFailure {
      code: "timeout",
      message: "no answer".into(),
    }),
  }
}

fn scratch_dir() -> Option<PathBuf> {
  let dir = std::env::temp_dir().join(format!(
    "laufey-e2e-webview-app-{}-{}",
    std::process::id(),
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .map(|d| d.as_nanos())
      .unwrap_or(0)
  ));
  std::fs::create_dir_all(&dir).ok()?;
  // The path the engines are given is the real one (macOS's /var is a link
  // to /private/var; a link in the path is fine, a link as the file is not).
  std::fs::canonicalize(&dir).ok()
}

pub async fn run(w: &Window) {
  let caps = laufey::webview_capabilities();
  if !caps.supported() {
    na("web view downloads / permission requests for the app (no web views)");
    return;
  }
  let Some(base) = serve() else {
    check("web view app checks: a loopback server", false);
    return;
  };
  downloads(w, &base).await;
  permissions(w, &base).await;
}

async fn downloads(w: &Window, base: &str) {
  let caps = laufey::webview_capabilities();
  let options = || WebViewOptions {
    url: format!("{base}/page"),
    bounds: bounds(),
    partition: Some("ephemeral:e2e-app".into()),
    downloads: Some("app".into()),
    ..Default::default()
  };
  if !caps.download_events() {
    check(
      "downloads: \"app\" is refused where the engine has no download events",
      laufey::create_webview(w.id(), &options(), |_| {}).err()
        == Some(WebViewError::Invalid),
    );
    na("web view download events (not supported by this engine)");
    return;
  }
  let Some(dir) = scratch_dir() else {
    check("web view app checks: a scratch directory", false);
    return;
  };
  let (events, sink) = recorder();
  let view = match laufey::create_webview(w.id(), &options(), sink) {
    Ok(view) => view,
    Err(e) => {
      check(&format!("a web view with downloads: \"app\" ({e})"), false);
      return;
    }
  };
  let loaded = wait_event(&events, WebViewEventKind::Load, 30, |d| {
    state_is(d, "complete")
  })
  .await
  .is_some();
  check("the app-downloads view loads its page", loaded);

  // 1. A small attachment: announced, nothing written, then saved to
  //    exactly the chosen path (over a file that was there).
  let _ = view.navigate(&format!("{base}/dl/small"));
  let requested = wait_event(&events, WebViewEventKind::Download, 30, |d| {
    state_is(d, "requested")
  })
  .await;
  check("a download is announced as requested", requested.is_some());
  let Some(requested) = requested else {
    view.close();
    let _ = std::fs::remove_dir_all(&dir);
    return;
  };
  let first = id_of(&requested);
  eprintln!("[e2e] INFO download requested: {requested}");
  check(
    "the requested event names the URL and a bare file name",
    first != 0
      && requested["url"].as_str() == Some(&format!("{base}/dl/small"))
      && requested["filename"].as_str() == Some("report.txt")
      && requested["receivedBytes"].as_u64() == Some(0)
      && requested.get("path").is_none(),
  );
  check(
    "totalBytes, where reported, is the response's length",
    requested
      .get("totalBytes")
      .is_none_or(|t| t.as_u64() == Some(SMALL.len() as u64)),
  );
  let target = dir.join("saved report.txt");
  let _ = std::fs::write(&target, b"an older file that must be replaced");
  tokio::time::sleep(Duration::from_millis(500)).await;
  check(
    "nothing is written while a download waits for the app",
    std::fs::read_dir(&dir).map(|d| d.count()).unwrap_or(0) == 1
      && states_of(&events, first) == ["requested"],
  );
  check(
    "a relative path is refused",
    view.download_control(first, DownloadAction::Save("report.txt"))
      == Err(WebViewError::Invalid),
  );
  check(
    "a directory is refused as the path",
    view.download_control(first, DownloadAction::Save(&dir.to_string_lossy()))
      == Err(WebViewError::Invalid),
  );
  check(
    "a path in a missing directory is refused",
    view.download_control(
      first,
      DownloadAction::Save(&dir.join("nope").join("x.txt").to_string_lossy()),
    ) == Err(WebViewError::Invalid),
  );
  check(
    "resume is refused for a download that waits for its answer",
    matches!(
      view.download_control(first, DownloadAction::Resume),
      Err(WebViewError::Invalid | WebViewError::Unsupported)
    ),
  );
  check(
    "an unknown download id is refused",
    view.download_control(first + 1_000_000, DownloadAction::Cancel)
      == Err(WebViewError::Invalid),
  );
  check(
    "SAVE is accepted for a requested download",
    view
      .download_control(first, DownloadAction::Save(&target.to_string_lossy()))
      == Ok(()),
  );
  let completed = wait_event(&events, WebViewEventKind::Download, 30, |d| {
    id_of(d) == first && state_is(d, "completed")
  })
  .await;
  check("the saved download completes", completed.is_some());
  if let Some(completed) = &completed {
    check(
      "completed reports the path and the size",
      completed["path"].as_str() == Some(&*target.to_string_lossy())
        && completed["receivedBytes"].as_u64() == Some(SMALL.len() as u64)
        && completed["url"] == requested["url"]
        && completed["filename"] == requested["filename"],
    );
  }
  check(
    "the file at the chosen path is the download (the old file is replaced)",
    std::fs::read(&target).ok().as_deref() == Some(SMALL.as_bytes()),
  );
  check(
    "nothing else was written next to it",
    std::fs::read_dir(&dir).map(|d| d.count()).unwrap_or(0) == 1,
  );
  let states = states_of(&events, first);
  check(
    "a download's events run requested, progressing*, completed",
    states.first().map(String::as_str) == Some("requested")
      && states.last().map(String::as_str) == Some("completed")
      && states[1..states.len() - 1]
        .iter()
        .all(|s| s == "progressing"),
  );
  check(
    "a finished download refuses further control",
    view.download_control(first, DownloadAction::Cancel)
      == Err(WebViewError::Invalid),
  );
  check(
    "the page stays after a navigation became a download",
    view
      .state()
      .is_some_and(|s| s.contains(&format!("{base}/page"))),
  );

  // 2. Cancelled before the answer: one "cancelled", nothing written.
  let _ = view.navigate(&format!("{base}/dl/small"));
  let second = wait_event(&events, WebViewEventKind::Download, 30, |d| {
    state_is(d, "requested") && id_of(d) != first
  })
  .await
  .map(|d| id_of(&d))
  .unwrap_or(0);
  check("a second download gets its own id", second != 0);
  if second != 0 {
    check(
      "CANCEL is accepted for a requested download",
      view.download_control(second, DownloadAction::Cancel) == Ok(()),
    );
    check(
      "a download cancelled before its answer reports cancelled",
      wait_event(&events, WebViewEventKind::Download, 15, |d| {
        id_of(d) == second && state_is(d, "cancelled")
      })
      .await
      .is_some()
        && states_of(&events, second) == ["requested", "cancelled"],
    );
    check(
      "a cancelled download refuses further control",
      view.download_control(second, DownloadAction::Cancel)
        == Err(WebViewError::Invalid),
    );
    check(
      "a download cancelled before its answer writes nothing",
      std::fs::read_dir(&dir).map(|d| d.count()).unwrap_or(0) == 1,
    );
  }

  // 3. A slow download: progress, pause / resume where reported, then a
  //    cancel part-way that leaves no file.
  let slow = dir.join("slow.bin");
  let _ = view.navigate(&format!("{base}/dl/slow"));
  let third = wait_event(&events, WebViewEventKind::Download, 30, |d| {
    state_is(d, "requested") && id_of(d) != first && id_of(d) != second
  })
  .await
  .map(|d| id_of(&d))
  .unwrap_or(0);
  check("the slow download is announced", third != 0);
  if third != 0 {
    let _ = view
      .download_control(third, DownloadAction::Save(&slow.to_string_lossy()));
    let progressed = wait_event(&events, WebViewEventKind::Download, 30, |d| {
      id_of(d) == third
        && state_is(d, "progressing")
        && d["receivedBytes"].as_u64().unwrap_or(0) > 0
    })
    .await;
    check(
      "a download in progress reports received bytes and its path",
      progressed.as_ref().is_some_and(|d| {
        d["path"].as_str() == Some(&*slow.to_string_lossy())
          && d["receivedBytes"].as_u64().unwrap_or(0)
            < (SLOW_CHUNK * SLOW_CHUNKS) as u64
      }),
    );
    if caps.download_pause() {
      check(
        "PAUSE is accepted for a progressing download",
        view.download_control(third, DownloadAction::Pause) == Ok(()),
      );
      let paused = wait_event(&events, WebViewEventKind::Download, 15, |d| {
        id_of(d) == third && d["paused"].as_bool() == Some(true)
      })
      .await;
      check("a paused download says so", paused.is_some());
      check(
        "PAUSE is refused for a paused download",
        view.download_control(third, DownloadAction::Pause)
          == Err(WebViewError::Invalid),
      );
      let at_pause = details(&events, WebViewEventKind::Download)
        .into_iter()
        .filter(|d| id_of(d) == third)
        .filter_map(|d| d["receivedBytes"].as_u64())
        .max()
        .unwrap_or(0);
      check(
        "RESUME is accepted for a paused download",
        view.download_control(third, DownloadAction::Resume) == Ok(()),
      );
      check(
        "a resumed download receives more",
        wait_event(&events, WebViewEventKind::Download, 30, |d| {
          id_of(d) == third
            && d.get("paused").is_none()
            && d["receivedBytes"].as_u64().unwrap_or(0) > at_pause
        })
        .await
        .is_some(),
      );
    } else {
      check(
        "PAUSE is unsupported where the engine has no pause",
        view.download_control(third, DownloadAction::Pause)
          == Err(WebViewError::Unsupported),
      );
    }
    check(
      "CANCEL is accepted for a progressing download",
      view.download_control(third, DownloadAction::Cancel) == Ok(()),
    );
    check(
      "a download cancelled part-way reports cancelled last",
      wait_event(&events, WebViewEventKind::Download, 15, |d| {
        id_of(d) == third && state_is(d, "cancelled")
      })
      .await
      .is_some()
        && states_of(&events, third).last().map(String::as_str)
          == Some("cancelled"),
    );
    check(
      "a download cancelled part-way leaves no file at its path",
      gone(&slow, 10).await,
    );
  }

  // 4. Closing the view cancels what is in flight, before CLOSED.
  let _ = view.navigate(&format!("{base}/dl/slow"));
  let fourth = wait_event(&events, WebViewEventKind::Download, 30, |d| {
    state_is(d, "requested") && ![first, second, third].contains(&id_of(d))
  })
  .await
  .map(|d| id_of(&d))
  .unwrap_or(0);
  let _ = view.navigate(&format!("{base}/dl/small"));
  let fifth = wait_event(&events, WebViewEventKind::Download, 30, |d| {
    state_is(d, "requested")
      && ![first, second, third, fourth].contains(&id_of(d))
  })
  .await
  .map(|d| id_of(&d))
  .unwrap_or(0);
  check(
    "two more downloads are announced before the view closes",
    fourth != 0 && fifth != 0,
  );
  let closing = dir.join("closing.bin");
  if fourth != 0 {
    let _ = view.download_control(
      fourth,
      DownloadAction::Save(&closing.to_string_lossy()),
    );
    let _ = wait_event(&events, WebViewEventKind::Download, 30, |d| {
      id_of(d) == fourth
        && state_is(d, "progressing")
        && d["receivedBytes"].as_u64().unwrap_or(0) > 0
    })
    .await;
  }
  view.close();
  let closed = wait_event(&events, WebViewEventKind::Closed, 15, |_| true)
    .await
    .is_some();
  check("the app-downloads view closes", closed);
  let order: Vec<(WebViewEventKind, String)> = events
    .lock()
    .unwrap()
    .iter()
    .map(|e| (e.kind, e.detail_json.clone()))
    .collect();
  let closed_at = order
    .iter()
    .position(|(k, _)| *k == WebViewEventKind::Closed)
    .unwrap_or(usize::MAX);
  let cancelled_before_closed = |id: u32| {
    order.iter().position(|(k, d)| {
      *k == WebViewEventKind::Download
        && serde_json::from_str::<serde_json::Value>(d)
          .is_ok_and(|d| id_of(&d) == id && state_is(&d, "cancelled"))
    }) < Some(closed_at)
      && states_of(&events, id).last().map(String::as_str) == Some("cancelled")
  };
  if fourth != 0 && fifth != 0 {
    check(
      "closing the view cancels a download in progress before CLOSED",
      cancelled_before_closed(fourth),
    );
    check(
      "closing the view cancels a download that waits before CLOSED",
      cancelled_before_closed(fifth),
    );
    check(
      "CLOSED is the view's last event",
      closed_at + 1 == order.len(),
    );
    check(
      "a download cancelled by the close leaves no file",
      gone(&closing, 10).await,
    );
  }
  check(
    "a closed view's downloads refuse control",
    view.download_control(fifth.max(1), DownloadAction::Cancel)
      == Err(WebViewError::Closed),
  );
  // Nothing fires after CLOSED.
  let before = events.lock().unwrap().len();
  tokio::time::sleep(Duration::from_millis(1500)).await;
  check(
    "no event follows CLOSED",
    events.lock().unwrap().len() == before,
  );
  let _ = std::fs::remove_dir_all(&dir);
}

/// A script whose completion value is a promise that settles once the
/// permission request is answered, and the permission it asks for.
fn permission_probe(
  engine: Option<WebViewEngine>,
) -> (&'static str, &'static str) {
  match engine {
    // WKWebView raises only capture requests to the embedder.
    Some(WebViewEngine::WkWebView) => (
      "navigator.mediaDevices.getUserMedia({audio: true})\
.then(s => { s.getTracks().forEach(t => t.stop()); return 'granted'; }, \
e => 'refused:' + e.name)",
      "microphone",
    ),
    _ => (
      "new Promise(r => navigator.geolocation.getCurrentPosition(\
() => r('granted'), e => r('refused:' + e.code), {timeout: 60000}))",
      "geolocation",
    ),
  }
}

async fn permissions(w: &Window, base: &str) {
  let caps = laufey::webview_capabilities();
  let options = || WebViewOptions {
    url: format!("{base}/page"),
    bounds: bounds(),
    partition: Some("ephemeral:e2e-app-perm".into()),
    permissions: Some("app".into()),
    ..Default::default()
  };
  if !caps.permission_requests() {
    check(
      "permissions: \"app\" is refused where the engine has no permission requests",
      laufey::create_webview(w.id(), &options(), |_| {}).err()
        == Some(WebViewError::Invalid),
    );
    na("web view permission requests (not supported by this engine)");
    return;
  }
  if !caps.evaluate() {
    na("web view permission requests (no evaluate to raise one)");
    return;
  }
  let (script, kind) = permission_probe(caps.engine());
  let (events, sink) = recorder();
  let view = match laufey::create_webview(w.id(), &options(), sink) {
    Ok(view) => view,
    Err(e) => {
      check(
        &format!("a web view with permissions: \"app\" ({e})"),
        false,
      );
      return;
    }
  };
  let loaded = wait_event(&events, WebViewEventKind::Load, 30, |d| {
    state_is(d, "complete")
  })
  .await
  .is_some();
  check("the app-permissions view loads its page", loaded);

  // 1. A request reaches the app; a refusal reaches the page.
  let asking = tokio::spawn(evaluate(view, script, 60));
  let requested =
    wait_event(&events, WebViewEventKind::PermissionRequest, 20, |d| {
      state_is(d, "requested")
    })
    .await;
  let Some(requested) = requested else {
    // No request: the engine answered by itself (no capture device in this
    // session, or the session refuses the capability outright).
    let answer = tokio::time::timeout(Duration::from_secs(5), asking).await;
    na(&format!(
      "web view permission requests (the engine raised none for {kind}: {answer:?})"
    ));
    view.close();
    return;
  };
  eprintln!("[e2e] INFO permission requested: {requested}");
  let first = id_of(&requested);
  check(
    "a permission request names its origin and what it asks for",
    first != 0
      && requested["origin"].as_str() == Some(base)
      && requested["permissions"]
        .as_array()
        .is_some_and(|p| p.iter().any(|k| k.as_str() == Some(kind))),
  );
  check(
    "the requesting frame, where reported, is the main frame of that origin",
    requested.get("frame").is_none_or(|f| {
      f["isMain"].as_bool() == Some(true)
        && f.get("origin").is_none_or(|o| o.as_str() == Some(base))
    }),
  );
  check(
    "an unknown permission request id is refused",
    view.permission_respond(first + 1_000_000, false)
      == Err(WebViewError::Invalid),
  );
  check(
    "refusing a permission request is accepted",
    view.permission_respond(first, false) == Ok(()),
  );
  let answer = tokio::time::timeout(Duration::from_secs(30), asking)
    .await
    .ok()
    .and_then(|r| r.ok())
    .and_then(|r| r.ok())
    .unwrap_or_default();
  eprintln!("[e2e] INFO the page's answer to a refusal: {answer}");
  check(
    "the page's request is refused when the app refuses",
    answer.contains("refused"),
  );
  check(
    "an answered permission request refuses a second answer",
    view.permission_respond(first, true) == Err(WebViewError::Invalid),
  );
  check(
    "no cancelled event follows an answer",
    !details(&events, WebViewEventKind::PermissionRequest)
      .iter()
      .any(|d| state_is(d, "cancelled")),
  );

  // 2. A request the page abandons (it navigates) is cancelled.
  let abandoned = tokio::spawn(evaluate(view, script, 20));
  let second =
    wait_event(&events, WebViewEventKind::PermissionRequest, 20, |d| {
      state_is(d, "requested") && id_of(d) != first
    })
    .await
    .map(|d| id_of(&d))
    .unwrap_or(0);
  if second == 0 {
    // Some engines remember a refusal for the page's lifetime.
    na("a second permission request (the engine did not ask again)");
  } else {
    let _ = view.navigate(&format!("{base}/page?again"));
    check(
      "a request the page navigates away from is reported cancelled",
      wait_event(&events, WebViewEventKind::PermissionRequest, 20, |d| {
        id_of(d) == second && state_is(d, "cancelled")
      })
      .await
      .is_some(),
    );
    check(
      "a cancelled permission request refuses an answer",
      view.permission_respond(second, true) == Err(WebViewError::Invalid),
    );
    let _ = wait_event(&events, WebViewEventKind::Load, 30, |d| {
      state_is(d, "complete")
        && d["url"].as_str().is_some_and(|u| u.ends_with("?again"))
    })
    .await;
  }
  abandoned.abort();

  // 3. Closing the view cancels a waiting request, before CLOSED.
  let closing = tokio::spawn(evaluate(view, script, 20));
  let third =
    wait_event(&events, WebViewEventKind::PermissionRequest, 20, |d| {
      state_is(d, "requested") && id_of(d) != first && id_of(d) != second
    })
    .await
    .map(|d| id_of(&d))
    .unwrap_or(0);
  view.close();
  let closed = wait_event(&events, WebViewEventKind::Closed, 15, |_| true)
    .await
    .is_some();
  check("the app-permissions view closes", closed);
  closing.abort();
  if third == 0 {
    na("a permission request waiting at close (the engine did not ask again)");
  } else {
    let order: Vec<(WebViewEventKind, String)> = events
      .lock()
      .unwrap()
      .iter()
      .map(|e| (e.kind, e.detail_json.clone()))
      .collect();
    let closed_at = order
      .iter()
      .position(|(k, _)| *k == WebViewEventKind::Closed)
      .unwrap_or(usize::MAX);
    let cancelled_at = order.iter().position(|(k, d)| {
      *k == WebViewEventKind::PermissionRequest
        && serde_json::from_str::<serde_json::Value>(d)
          .is_ok_and(|d| id_of(&d) == third && state_is(&d, "cancelled"))
    });
    check(
      "closing the view cancels a waiting permission request before CLOSED",
      cancelled_at.is_some_and(|at| at < closed_at)
        && closed_at + 1 == order.len(),
    );
  }
  check(
    "a closed view's permission requests refuse an answer",
    view.permission_respond(third.max(1), false) == Err(WebViewError::Closed),
  );
}
