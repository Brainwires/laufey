// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Microphone and camera requests (API 53; docs/media-capture.md). Run with
//! `LAUFEY_E2E_ONLY=media-capture` by `native-e2e-run.sh --media-capture`,
//! which writes a laufey-launch.json granting both to `app://e2e-media-ok`
//! and gives the engine stand-in devices (CEF and WebView2
//! `--use-fake-device-for-media-stream`, WebKit's mock capture devices).
//! Where the engine still has no device (the granted origin's capture ends in
//! NotFoundError: an engine answers that before it asks anyone), every check
//! that needs the request to reach the host is N/A.
//!
//! - The granted origin captures without the runtime being asked.
//! - Another origin is refused while no handler is set (the default).
//! - A sub-frame of another origin inside the granted page is refused, even
//!   though its parent delegates the devices to it (`allow=`). WebKitGTK names
//!   no frame, so there the top-level document's grant reaches it (N/A).
//! - With a handler, a request the file doesn't grant is asked with its
//!   origin and kinds, and the answer reaches the page (allowed: captures;
//!   refused: NotAllowedError). The granted origin still isn't asked.
//! - A request the handler holds is refused once its window navigates to
//!   another document: a late yes reports `false`.
//! - An answer for a request that isn't waiting reports `false`.
//! - WebRTC: a page that captures nothing makes an RTCPeerConnection loopback
//!   (an offer; no host candidate names the host's address), and the granted
//!   page sends its microphone over one (an audio offer, a candidate). N/A on
//!   a WebKitGTK built without WebRTC unless LAUFEY_E2E_EXPECT_WEBRTC=1.
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
/// "err:<name>". The "rtc" step makes an RTCPeerConnection loopback (two
/// connections in the page, a data channel, offer and answer) and gathers the
/// offerer's ICE candidates; "rtcmic" also sends a captured microphone track.
/// Either reports "ok:" and a JSON object (`offer`, `audio`: the offer has a
/// media section / an audio one; `candidates`: each candidate line). A `?ping`
/// page reports `<tag>ping` "ok" as soon as it runs. A
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
async function rtc(withMic) {
  if (typeof RTCPeerConnection === 'undefined')
    return 'err:NoRTCPeerConnection';
  let a, b, stream;
  try {
    a = new RTCPeerConnection();
    b = new RTCPeerConnection();
    const candidates = [];
    const gathered = new Promise((resolve) => {
      a.onicecandidate = (e) => {
        if (!e.candidate) return resolve();
        if (e.candidate.candidate) candidates.push(e.candidate.candidate);
        b.addIceCandidate(e.candidate).catch(() => {});
      };
      setTimeout(resolve, 10000);
    });
    b.onicecandidate = (e) => {
      if (e.candidate) a.addIceCandidate(e.candidate).catch(() => {});
    };
    if (withMic) {
      stream = await navigator.mediaDevices.getUserMedia({audio: true});
      stream.getTracks().forEach((t) => a.addTrack(t, stream));
    }
    a.createDataChannel('e2e');
    const offer = await a.createOffer();
    await a.setLocalDescription(offer);
    await b.setRemoteDescription(offer);
    await b.setLocalDescription(await b.createAnswer());
    await a.setRemoteDescription(b.localDescription);
    await gathered;
    const sdp = offer.sdp || '';
    return 'ok:' + JSON.stringify({offer: /^m=/m.test(sdp),
                                   audio: /^m=audio/m.test(sdp), candidates});
  } catch (e) {
    return 'err:' + (e && e.name ? e.name : String(e));
  } finally {
    if (stream) stream.getTracks().forEach((t) => t.stop());
    if (a) a.close();
    if (b) b.close();
  }
}
function run(step) {
  return step === 'rtc' || step === 'rtcmic' ? rtc(step === 'rtcmic')
                                             : capture(step);
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
  if (params.has('ping')) report('ping', 'ok');
  (async () => {
    for (const step of (params.get('steps') || '').split(',').filter(Boolean))
      report(step, await run(step));
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

/// An "rtc" / "rtcmic" report: whether the offer has a media section and an
/// audio one, and the offerer's candidate lines. None for an error.
fn rtc_report(result: &str) -> Option<(bool, bool, Vec<String>)> {
  let v: serde_json::Value =
    serde_json::from_str(result.strip_prefix("ok:")?).ok()?;
  let candidates = v["candidates"]
    .as_array()?
    .iter()
    .filter_map(|c| c.as_str().map(str::to_string))
    .collect();
  Some((v["offer"].as_bool()?, v["audio"].as_bool()?, candidates))
}

/// A host candidate whose address is the machine's own IP address, not an
/// mDNS name (`<uuid>.local`). Fields: foundation, component, protocol,
/// priority, address, port, "typ", type.
fn host_address_exposed(candidate: &str) -> bool {
  let f: Vec<&str> = candidate.split_whitespace().collect();
  f.get(6) == Some(&"typ")
    && f.get(7) == Some(&"host")
    && f
      .get(4)
      .is_some_and(|a| a.parse::<std::net::IpAddr>().is_ok())
}

fn webkit_gtk() -> bool {
  cfg!(target_os = "linux")
    && std::env::var("LAUFEY_E2E_BACKEND").as_deref() == Ok("webview")
}

/// N/A for a page with no RTCPeerConnection on a WebKitGTK built without
/// WebRTC (ENABLE_WEB_RTC and USE_GSTREAMER_WEBRTC, which distribution builds
/// leave off): laufey's setting then has nothing to turn on. With
/// LAUFEY_E2E_EXPECT_WEBRTC=1 (a WebKitGTK that has it) that is a failure, as
/// it is on every other backend.
fn webrtc_missing_na(name: &str, result: &str) -> bool {
  if result == "err:NoRTCPeerConnection"
    && webkit_gtk()
    && std::env::var("LAUFEY_E2E_EXPECT_WEBRTC").as_deref() != Ok("1")
  {
    na(&format!("{name} (this WebKitGTK is built without WebRTC)"));
    return true;
  }
  false
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

  // --- WebRTC with no capture ---------------------------------------------
  // A page that captures nothing makes an RTCPeerConnection loopback: it
  // creates an offer, and the host's own addresses stay hidden (WebKit's ICE
  // candidate filtering, mDNS names).
  let rtc_win = window(&format!("{NO_ORIGIN}/?tag=rtc-&steps=rtc"), &reports);
  let r = result_of(&reports, NO_ORIGIN, "rtc-rtc").await;
  rtc_win.close();
  match rtc_report(&r) {
    _ if webrtc_missing_na(
      "media-capture: an RTCPeerConnection loopback",
      &r,
    ) => {}
    Some((offer, _, candidates)) => {
      check(
        &format!("media-capture: an RTCPeerConnection loopback creates an offer ({candidates:?})"),
        offer,
      );
      check(
        &format!("media-capture: no ICE host candidate names the host's address before any capture ({candidates:?})"),
        !candidates.iter().any(|c| host_address_exposed(c)),
      );
    }
    None => check(
      &format!("media-capture: an RTCPeerConnection loopback ({r})"),
      false,
    ),
  }

  // --- No handler: the launch file decides, everything else is refused ----
  let frame_src = format!("{FRAME_ORIGIN}/");
  let ok_win = window(
    &format!("{OK_ORIGIN}/?steps=audio,video,both,rtcmic&frame={frame_src}"),
    &reports,
  );
  let mut no_device = false;
  for step in ["audio", "video", "both"] {
    let r = result_of(&reports, OK_ORIGIN, step).await;
    no_device = no_device || r == "err:NotFoundError";
    check_allowed(
      &format!(
        "media-capture: the granted origin captures {step} without asking"
      ),
      &r,
    );
  }
  if no_device {
    na("media-capture: the refusal and handler checks (no capture device: the engine answers NotFoundError before it asks the host)");
    ok_win.close();
    check(
      "media-capture: an answer for an unknown request reports false",
      !laufey::answer_media_capture(u64::MAX, true),
    );
    return;
  }
  // A voice call's shape: the granted microphone's track on a loopback,
  // which offers an audio section and gathers a candidate.
  let r = result_of(&reports, OK_ORIGIN, "rtcmic").await;
  if refused(&r) && cfg!(target_os = "macos") {
    na(&format!("media-capture: WebRTC with the granted microphone ({r}: macOS's own consent refused the test host)"));
  } else if !webrtc_missing_na(
    "media-capture: WebRTC with the granted microphone",
    &r,
  ) {
    let ok = rtc_report(&r)
      .is_some_and(|(offer, audio, c)| offer && audio && !c.is_empty());
    check(
      &format!("media-capture: an RTCPeerConnection loopback sends the granted microphone and gathers a candidate ({r})"),
      ok,
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

  // --- A request whose document is gone -----------------------------------
  // The handler holds the request; the window then navigates to another
  // origin. Once the new page runs, the old request is refused: a late yes
  // reaches nothing.
  let held: Arc<Mutex<Vec<MediaCaptureRequest>>> =
    Arc::new(Mutex::new(Vec::new()));
  let h = held.clone();
  laufey::on_media_capture_request(move |req| h.lock().unwrap().push(req));
  let hold_win =
    window(&format!("{ASK_ORIGIN}/?tag=held-&steps=audio"), &reports);
  let hold_id = hold_win.id();
  let held_req = || {
    held
      .lock()
      .unwrap()
      .iter()
      .find(|r| r.window_id == hold_id && r.origin == ASK_ORIGIN)
      .map(|r| r.id)
  };
  wait_for(|| held_req().is_some(), 300, 100).await;
  match held_req() {
    Some(id) => {
      hold_win.navigate(&format!("{NO_ORIGIN}/?tag=after-&ping"));
      let ping = result_of(&reports, NO_ORIGIN, "after-ping").await;
      check(
        &format!("media-capture: a request is refused once its window's top-level document is replaced ({ping})"),
        ping == "ok" && !laufey::answer_media_capture(id, true),
      );
    }
    None => check(
      "media-capture: the handler holds a request (never asked)",
      false,
    ),
  }
  hold_win.close();

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
