// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Microphone and camera requests (API 53; docs/media-capture.md). Run with
//! `LAUFEY_E2E_ONLY=media-capture` by `native-e2e-run.sh --media-capture`,
//! which writes a laufey-launch.json granting both to `app://e2e-media-ok`
//! and gives the engine stand-in devices (CEF and WebView2
//! `--use-fake-device-for-media-stream`, WebKit's mock capture devices).
//!
//! - The granted origin captures without the runtime being asked.
//! - Another origin is refused while no handler is set (the default).
//! - A sub-frame of another origin inside the granted page is refused, even
//!   though its parent delegates the devices to it (`allow=`). WebKitGTK names
//!   no frame, so there the top-level document's grant reaches it (N/A).
//! - With a handler, a request the file doesn't grant is asked with its
//!   origin and kinds, and the answer reaches the page (allowed: captures;
//!   refused: NotAllowedError). The granted origin still isn't asked.
//! - An answer for a request that isn't waiting reports `false`.
//!
//! An allowed request still meets the OS's consent: on macOS (TCC) an
//! unsigned test host may be refused there, which the page can't tell from
//! laufey's refusal, so an allowed request that ends in NotAllowedError is
//! N/A on macOS.

use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{
  MediaCaptureKinds, MediaCaptureRequest, SchemeRequest, Value, Window,
};

use super::{arg_string, check, na, respond, wait_for};

pub const OK_ORIGIN: &str = "app://e2e-media-ok";
const NO_ORIGIN: &str = "app://e2e-media-no";
const ASK_ORIGIN: &str = "app://e2e-media-ask";
const FRAME_ORIGIN: &str = "app://e2e-media-frame";

/// Captures what `steps` names ("audio", "video", "both") one after another
/// and reports each result through the bridge (as `<tag><step>`): "ok", or
/// "err:<name>". A
/// `?frame` page also embeds FRAME_ORIGIN's capturing page, which posts its
/// result to its parent.
const PAGE_HTML: &str = r#"<!doctype html><html><head><meta charset="utf-8">
<title>media capture</title></head><body>media
<script>
const constraints = {audio: {audio: true}, video: {video: true},
                     both: {audio: true, video: true}};
async function capture(step) {
  try {
    if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia)
      return 'err:NoMediaDevices';
    const stream = await navigator.mediaDevices.getUserMedia(constraints[step]);
    stream.getTracks().forEach((t) => t.stop());
    return 'ok';
  } catch (e) {
    return 'err:' + (e && e.name ? e.name : String(e));
  }
}
function report(step, result) {
  const send = () => {
    if (typeof Laufey === 'undefined') return setTimeout(send, 100);
    Laufey.mediaReport(String(location.origin), tag + step, result)
      .catch(() => {});
  };
  send();
}
const params = new URLSearchParams(location.search);
const tag = params.get('tag') || '';
if (window.top !== window) {
  capture('audio').then((r) => parent.postMessage({frame: r}, '*'));
} else {
  window.addEventListener('message', (e) => {
    if (e.data && typeof e.data.frame === 'string') report('frame', e.data.frame);
  });
  if (params.has('frame')) {
    const f = document.createElement('iframe');
    f.allow = 'microphone; camera';
    f.src = params.get('frame');
    document.body.appendChild(f);
  }
  (async () => {
    for (const step of (params.get('steps') || '').split(',').filter(Boolean))
      report(step, await capture(step));
  })();
}
</script></body></html>"#;

/// Serves the pages; hands every other request back.
pub fn serve(req: SchemeRequest) -> Option<SchemeRequest> {
  let path = req.url.split(['?', '#']).next().unwrap_or("").to_string();
  let path = path.trim_end_matches('/');
  if [OK_ORIGIN, NO_ORIGIN, ASK_ORIGIN, FRAME_ORIGIN].contains(&path) {
    respond(req, 200, "text/html; charset=utf-8", PAGE_HTML.as_bytes());
    return None;
  }
  Some(req)
}

/// (page origin, step, result)
type Reports = Arc<Mutex<Vec<(String, String, String)>>>;

fn window(url: &str, reports: &Reports) -> Window {
  let reports = reports.clone();
  Window::new(480, 320)
    .bind("mediaReport", move |call| {
      reports.lock().unwrap().push((
        arg_string(&call.args, 0),
        arg_string(&call.args, 1),
        arg_string(&call.args, 2),
      ));
      call.resolve(Value::Bool(true));
    })
    .load(url)
}

async fn result_of(reports: &Reports, origin: &str, step: &str) -> String {
  let found = || {
    reports
      .lock()
      .unwrap()
      .iter()
      .find(|(o, s, _)| o == origin && s == step)
      .map(|(_, _, r)| r.clone())
  };
  wait_for(|| found().is_some(), 300, 100).await;
  found().unwrap_or_else(|| "none".to_string())
}

fn refused(result: &str) -> bool {
  result == "err:NotAllowedError"
}

/// An allowed request: it captured, or failed for another reason than a
/// refusal (no device). NotAllowedError on macOS is the OS's consent (N/A).
fn check_allowed(name: &str, result: &str) {
  if refused(result) && cfg!(target_os = "macos") {
    na(&format!(
      "{name} ({result}: macOS's own consent refused the test host)"
    ));
    return;
  }
  check(
    &format!("{name} ({result})"),
    result != "none" && !refused(result) && result != "err:NoMediaDevices",
  );
}

fn webkit_gtk() -> bool {
  cfg!(target_os = "linux")
    && std::env::var("LAUFEY_E2E_BACKEND").as_deref() == Ok("webview")
}

pub async fn run() {
  if !laufey::scheme_handlers_supported() {
    na("media-capture (backend has no web engine)");
    return;
  }
  // The windows open one after another, each closed before the next: the
  // app must outlive its last window in between.
  laufey::set_quit_on_last_window_closed(false);
  let reports: Reports = Arc::new(Mutex::new(Vec::new()));

  // --- No handler: the launch file decides, everything else is refused ----
  let frame_src = format!("{FRAME_ORIGIN}/");
  let ok_win = window(
    &format!("{OK_ORIGIN}/?steps=audio,video,both&frame={frame_src}"),
    &reports,
  );
  for step in ["audio", "video", "both"] {
    let r = result_of(&reports, OK_ORIGIN, step).await;
    check_allowed(
      &format!(
        "media-capture: the granted origin captures {step} without asking"
      ),
      &r,
    );
  }
  let r = result_of(&reports, OK_ORIGIN, "frame").await;
  if webkit_gtk() {
    na(&format!("media-capture: a sub-frame of another origin (WebKitGTK names no frame: {r})"));
  } else {
    check(
      &format!("media-capture: a sub-frame of another origin is refused, though its parent delegates ({r})"),
      refused(&r),
    );
  }
  ok_win.close();

  let no_win = window(&format!("{NO_ORIGIN}/?steps=audio"), &reports);
  let r = result_of(&reports, NO_ORIGIN, "audio").await;
  check(
    &format!("media-capture: another origin is refused by default ({r})"),
    refused(&r),
  );
  no_win.close();

  // --- A handler: asked about the rest, its answer reaches the page --------
  let asked: Arc<Mutex<Vec<MediaCaptureRequest>>> =
    Arc::new(Mutex::new(Vec::new()));
  let a = asked.clone();
  let supported = laufey::on_media_capture_request(move |req| {
    // The microphone for ASK_ORIGIN; nothing else.
    let allow =
      req.origin == ASK_ORIGIN && req.kinds == MediaCaptureKinds::MICROPHONE;
    a.lock().unwrap().push(req.clone());
    laufey::answer_media_capture(req.id, allow);
  });
  check("media-capture: the backend takes a handler", supported);

  let ask_win = window(&format!("{ASK_ORIGIN}/?steps=audio,video"), &reports);
  let audio = result_of(&reports, ASK_ORIGIN, "audio").await;
  let video = result_of(&reports, ASK_ORIGIN, "video").await;
  let seen = asked.lock().unwrap().clone();
  check(
    &format!("media-capture: the handler is asked with the origin and kinds ({seen:?})"),
    seen.iter().any(|r| {
      r.origin == ASK_ORIGIN
        && r.kinds == MediaCaptureKinds::MICROPHONE
        && r.window_id == ask_win.id()
    }) && seen.iter().any(|r| r.origin == ASK_ORIGIN && r.kinds.camera()),
  );
  check_allowed("media-capture: the handler's yes reaches the page", &audio);
  check(
    &format!("media-capture: the handler's no reaches the page ({video})"),
    refused(&video),
  );
  ask_win.close();

  // The granted origin still isn't asked; the other-origin frame never is.
  let ask_ids = asked_ids(&asked);
  asked.lock().unwrap().clear();
  let ok_again = window(
    &format!("{OK_ORIGIN}/?tag=again-&steps=audio&frame={frame_src}"),
    &reports,
  );
  let r = result_of(&reports, OK_ORIGIN, "again-audio").await;
  let f = result_of(&reports, OK_ORIGIN, "again-frame").await;
  let seen = asked.lock().unwrap().clone();
  check_allowed(
    "media-capture: the granted origin captures with a handler set",
    &r,
  );
  check(
    &format!("media-capture: the handler isn't asked for the granted origin ({seen:?})"),
    !seen.iter().any(|r| r.origin == OK_ORIGIN),
  );
  if webkit_gtk() {
    na(&format!("media-capture: the handler and another origin's sub-frame (WebKitGTK names no frame: {f})"));
  } else {
    check(
      &format!("media-capture: the handler is never asked for another origin's sub-frame ({f}, {seen:?})"),
      refused(&f) && !seen.iter().any(|r| r.origin == FRAME_ORIGIN),
    );
  }
  ok_again.close();

  // --- Answers for requests that aren't waiting ---------------------------
  tokio::time::sleep(Duration::from_millis(300)).await;
  check(
    "media-capture: an answer for an unknown request reports false",
    !laufey::answer_media_capture(u64::MAX, true),
  );
  check(
    &format!("media-capture: a request answers once ({ask_ids:?})"),
    !ask_ids.is_empty()
      && ask_ids
        .iter()
        .all(|id| !laufey::answer_media_capture(*id, true)),
  );
  laufey::clear_media_capture_handler();
}

fn asked_ids(asked: &Arc<Mutex<Vec<MediaCaptureRequest>>>) -> Vec<u64> {
  asked.lock().unwrap().iter().map(|r| r.id).collect()
}
