// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Page-triggered downloads (docs/downloads.md, laufey_downloads.h). Run with
//! `LAUFEY_E2E_ONLY=downloads` by `native-e2e-run.sh --downloads`.
//!
//! A window navigated to a loopback server (`http://127.0.0.1:<port>`, its
//! own origin). Every save dialog is the backend's own file dialog, answered
//! by `test_file_dialog_respond` as in the I/O battery:
//!
//! - a blob `<a download>` export (the upstream PR's case) asks, and the
//!   bytes land at the path the dialog returned;
//! - a same-origin `Content-Disposition: attachment` response whose filename
//!   climbs out of its directory (`../../…`) asks too, and writes only the
//!   chosen path: nothing at the name the server proposed, anywhere;
//! - dismissing the dialog writes nothing (and nothing goes to the Downloads
//!   folder instead), and the window lives on;
//! - a cross-origin iframe (`http://localhost:<port>`) can't start one:
//!   neither a blob export of its own nor an attachment it loads opens a
//!   dialog;
//! - a download link or attachment navigation to another origin in the top
//!   frame never reaches the save dialog: the navigation guard keeps the page
//!   and hands the navigation to the OS browser (captured);
//! - a page looping downloads opens at most 3 dialogs (one at a time);
//! - a 32 MiB attachment arrives whole.

use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{
  FileDialogKind, FileDialogOptions, FileDialogOutcome, Value, Window,
};

use super::{check, na, wait_for};

const BIG_LEN: usize = 32 * 1024 * 1024;
/// laufey_downloads.h's kDownloadPromptWindow, plus slack.
const BUDGET_RESET: Duration = Duration::from_millis(10_500);

type Hits = Arc<Mutex<Vec<String>>>;

fn big_byte(i: usize) -> u8 {
  ((i * 31 + i / 4096) % 251) as u8
}

fn path_of(head: &str) -> String {
  head
    .lines()
    .next()
    .and_then(|l| l.split_whitespace().nth(1))
    .unwrap_or("")
    .to_string()
}

fn page(body: &str) -> String {
  format!(
    "<!doctype html><html><head><meta charset=\"utf-8\">\
     <title>downloads</title></head><body>{body}</body></html>"
  )
}

/// A blob `<a download>` export of `text` as `name`, from a script.
fn blob_export_js(name: &str, text: &str) -> String {
  format!(
    "(function() {{ const b = new Blob([{text:?}], {{ type: 'text/plain' }}); \
     const a = document.createElement('a'); a.href = URL.createObjectURL(b); \
     a.download = {name:?}; document.body.appendChild(a); a.click(); \
     return 'ok'; }})()"
  )
}

fn respond_head(
  stream: &mut TcpStream,
  status: &str,
  headers: &str,
  len: usize,
) {
  let head = format!(
    "HTTP/1.1 {status}\r\n{headers}cache-control: no-store\r\n\
     content-length: {len}\r\nconnection: close\r\n\r\n"
  );
  let _ = stream.write_all(head.as_bytes());
}

fn serve(mut stream: TcpStream, port: u16, nonce: &str, hits: &Hits) {
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
  let html = "content-type: text/html; charset=utf-8\r\n";
  match path.as_str() {
    "/attach.bin" | "/frame-attach.bin" => {
      let body = format!("attached-{nonce}");
      respond_head(
        &mut stream,
        "200 OK",
        &format!(
          "content-type: application/octet-stream\r\n\
           content-disposition: attachment; \
           filename=\"../../laufey-dl-evil-{nonce}.txt\"\r\n"
        ),
        body.len(),
      );
      let _ = stream.write_all(body.as_bytes());
    }
    "/big.bin" => {
      respond_head(
        &mut stream,
        "200 OK",
        "content-type: application/octet-stream\r\n\
         content-disposition: attachment; filename=\"big.bin\"\r\n",
        BIG_LEN,
      );
      let mut block = vec![0u8; 64 * 1024];
      let mut sent = 0;
      while sent < BIG_LEN {
        for (k, b) in block.iter_mut().enumerate() {
          *b = big_byte(sent + k);
        }
        if stream.write_all(&block).is_err() {
          return;
        }
        sent += block.len();
      }
    }
    // A cross-origin frame that exports a blob of its own as soon as it
    // loads (no script of the test reaches into it).
    "/frame-blob" => {
      let body = page(&format!(
        "<script>{}</script>",
        blob_export_js(&format!("frame-{nonce}.txt"), "from the frame")
      ));
      respond_head(&mut stream, "200 OK", html, body.len());
      let _ = stream.write_all(body.as_bytes());
    }
    "/favicon.ico" => respond_head(&mut stream, "404 Not Found", "", 0),
    _ => {
      let body = page(&format!("downloads {port}"));
      respond_head(&mut stream, "200 OK", html, body.len());
      let _ = stream.write_all(body.as_bytes());
    }
  }
}

fn start_server(nonce: String) -> Option<(u16, Hits)> {
  let listener = TcpListener::bind("127.0.0.1:0").ok()?;
  let port = listener.local_addr().ok()?.port();
  let hits: Hits = Arc::new(Mutex::new(Vec::new()));
  let server_hits = hits.clone();
  std::thread::spawn(move || {
    for stream in listener.incoming().flatten() {
      let hits = server_hits.clone();
      let nonce = nonce.clone();
      std::thread::spawn(move || serve(stream, port, &nonce, &hits));
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

/// Answers the next save dialog within `tries` × 100 ms. False when none
/// opened.
async fn answer_dialog(accept: bool, path: Option<&Path>, tries: u32) -> bool {
  let path = path.map(|p| p.to_string_lossy().into_owned());
  for _ in 0..tries {
    if laufey::test_file_dialog_respond(accept, path.as_deref()) {
      return true;
    }
    tokio::time::sleep(Duration::from_millis(100)).await;
  }
  false
}

/// Waits (up to `secs`) for `path` to hold exactly `want`.
async fn wait_file(path: &Path, want: &[u8], secs: u64) -> bool {
  for _ in 0..secs * 5 {
    if let Ok(meta) = std::fs::metadata(path) {
      if meta.len() == want.len() as u64
        && std::fs::read(path).map(|b| b == want).unwrap_or(false)
      {
        return true;
      }
    }
    tokio::time::sleep(Duration::from_millis(200)).await;
  }
  false
}

/// Every directory a download might have gone to unasked, or that a
/// traversing name might reach from `dir`.
fn stray_dirs(dir: &Path) -> Vec<PathBuf> {
  let mut out = vec![dir.to_path_buf()];
  if let Some(p) = dir.parent() {
    out.push(p.to_path_buf());
    if let Some(pp) = p.parent() {
      out.push(pp.to_path_buf());
    }
  }
  if let Ok(cwd) = std::env::current_dir() {
    out.push(cwd);
  }
  for var in ["HOME", "USERPROFILE"] {
    if let Some(home) = std::env::var_os(var) {
      let home = PathBuf::from(home);
      out.push(home.join("Downloads"));
      out.push(home.clone());
    }
  }
  if let Some(xdg) = std::env::var_os("XDG_DOWNLOAD_DIR") {
    out.push(PathBuf::from(xdg));
  }
  out
}

/// Files under the stray directories whose name contains `needle`.
fn strays(dir: &Path, needle: &str) -> Vec<PathBuf> {
  let mut found = Vec::new();
  for d in stray_dirs(dir) {
    if let Ok(entries) = std::fs::read_dir(&d) {
      for e in entries.flatten() {
        if e.file_name().to_string_lossy().contains(needle) {
          found.push(e.path());
        }
      }
    }
  }
  found
}

fn dir_names(dir: &Path) -> Vec<String> {
  std::fs::read_dir(dir)
    .map(|it| {
      it.flatten()
        .map(|e| e.file_name().to_string_lossy().into_owned())
        .collect()
    })
    .unwrap_or_default()
}

/// A save step's check: the dialog must have asked; the write is checked
/// where the hook can accept a save dialog and N/A where it can't.
fn saved_check(name: &str, can_accept: bool, asked: bool, written: bool) {
  if !asked {
    check(&format!("{name} (a save dialog asked)"), false);
  } else if can_accept {
    check(name, written);
  } else {
    na(&format!(
      "{name} (this save dialog can't be accepted programmatically)"
    ));
  }
}

/// Whether test_file_dialog_respond can accept a save dialog on this
/// machine: an app save dialog in `dir`, accepted with a path in it.
async fn probe_accept(
  win: &Window,
  caps: &laufey::WindowCapabilities,
  dir: &Path,
) -> bool {
  let parent = if caps.file_dialog_modal() {
    win.id()
  } else {
    0
  };
  let d = laufey::show_file_dialog(
    parent,
    &FileDialogOptions {
      kind: FileDialogKind::Save,
      default_path: Some(dir.to_string_lossy().into_owned()),
      ..Default::default()
    },
  );
  let target = dir.join("probe.txt");
  let acted = answer_dialog(true, Some(&target), 100).await;
  let outcome = tokio::time::timeout(Duration::from_secs(60), d.outcome)
    .await
    .unwrap_or(FileDialogOutcome::Failed);
  acted && matches!(outcome, FileDialogOutcome::Accepted(_))
}

pub async fn run() {
  if !laufey::scheme_handlers_supported() {
    na("downloads (backend has no web engine)");
    return;
  }
  let caps = laufey::window_capabilities();
  if !caps.file_dialogs() {
    check("downloads: the backend has file dialogs", false);
    return;
  }
  let nonce = format!("{}-{}", std::process::id(), {
    std::time::SystemTime::now()
      .duration_since(std::time::UNIX_EPOCH)
      .map(|d| d.as_millis() % 1_000_000)
      .unwrap_or(0)
  });
  let dir = std::env::temp_dir().join(format!("laufey-dl-e2e-{nonce}"));
  if std::fs::create_dir_all(&dir).is_err() {
    check("downloads: a scratch directory", false);
    return;
  }
  // The dialogs propose names in a directory of their own choosing; the
  // checks always hand them a full path inside `dir`. (Not canonicalized: on
  // Windows that is a \\?\ path, which the save dialog won't take.)
  // The save dialogs open in `dir` (laufey_downloads.h's test hook), as the
  // I/O battery's do, so the typed path is in the directory shown.
  std::env::set_var("LAUFEY_TEST_DOWNLOAD_DIR", &dir);
  let Some((port, hits)) = start_server(nonce.clone()) else {
    check("downloads: a loopback server for the checks", false);
    return;
  };
  let own = format!("http://127.0.0.1:{port}");
  let other = format!("http://localhost:{port}");

  let win = Window::new(480, 320).title("native-e2e-downloads");
  win.show();
  win.navigate(&format!("{own}/"));
  let loaded =
    wait_for(|| hits.lock().unwrap().iter().any(|p| p == "/"), 100, 100).await;
  let ready = eval_string(&win, "String(location.origin)").await;
  check(
    &format!("downloads: the window shows the app's page ({ready:?})"),
    loaded && ready.as_deref() == Some(own.as_str()),
  );

  // Can the test hook accept a save dialog here at all? macOS runners show
  // the panel out of process, where it can't (as in the I/O battery, whose
  // accept steps then report N/A). Probe once with an app save dialog.
  let can_accept = probe_accept(&win, &caps, &dir).await;
  if !can_accept {
    eprintln!(
      "[e2e] INFO save dialogs can't be accepted programmatically here: \
       the accept-and-write steps check the dialog and that nothing is \
       written elsewhere, and report the write itself N/A"
    );
  }

  // --- 1. A blob <a download> export (the upstream PR's case). -------------
  let blob_text = format!("blob-export-{nonce}");
  let blob_out = dir.join("blob-out.txt");
  eval_string(
    &win,
    &blob_export_js(&format!("report-{nonce}.txt"), &blob_text),
  )
  .await;
  // The first save panel of a fresh macOS runner takes a while to come up.
  let asked = answer_dialog(true, Some(&blob_out), 100).await;
  let written = asked && wait_file(&blob_out, blob_text.as_bytes(), 60).await;
  saved_check(
    &format!(
      "downloads: a blob <a download> asks where, and saves there \
       (asked {asked}, in dir: {:?})",
      dir_names(&dir)
    ),
    can_accept,
    asked,
    written,
  );

  // --- 2. An attachment whose filename climbs out of its directory. --------
  let attach_out = dir.join("attach-out.bin");
  eval_string(&win, "location.href = '/attach.bin'; 'ok'").await;
  let asked = answer_dialog(true, Some(&attach_out), 100).await;
  let written = asked
    && wait_file(&attach_out, format!("attached-{nonce}").as_bytes(), 20).await;
  tokio::time::sleep(Duration::from_millis(500)).await;
  let evil = strays(&dir, &format!("laufey-dl-evil-{nonce}"));
  check(
    &format!(
      "downloads: a Content-Disposition filename with ../ writes nothing \
       under the proposed name (asked {asked}, strays {evil:?})"
    ),
    asked && evil.is_empty(),
  );
  saved_check(
    "downloads: ... and the chosen path is written",
    can_accept,
    asked,
    written,
  );
  let page_kept = eval_string(&win, "String(location.origin)").await;
  check(
    &format!(
      "downloads: the attachment left the page in place ({page_kept:?})"
    ),
    page_kept.as_deref() == Some(own.as_str()),
  );

  // --- 3. Dismissing the dialog. -------------------------------------------
  let cancel_name = format!("cancelled-{nonce}.txt");
  eval_string(&win, &blob_export_js(&cancel_name, "never written")).await;
  let asked = answer_dialog(false, None, 100).await;
  tokio::time::sleep(Duration::from_millis(1500)).await;
  let stray = strays(&dir, &format!("cancelled-{nonce}"));
  let alive = eval_string(&win, "'alive'").await;
  check(
    &format!(
      "downloads: dismissing the dialog writes nothing anywhere (asked \
       {asked}, strays {stray:?}, page {alive:?})"
    ),
    asked && stray.is_empty() && alive.as_deref() == Some("alive"),
  );

  // --- 4. A cross-origin iframe can't download. ----------------------------
  eval_string(
    &win,
    &format!(
      "for (const src of ['{other}/frame-blob', '{other}/frame-attach.bin']) \
       {{ const f = document.createElement('iframe'); f.src = src; \
       document.body.appendChild(f); }} 'ok'"
    ),
  )
  .await;
  let framed = wait_for(
    || {
      let h = hits.lock().unwrap();
      h.iter().any(|p| p == "/frame-blob")
        && h.iter().any(|p| p == "/frame-attach.bin")
    },
    50,
    200,
  )
  .await;
  // Any dialog that opens is answered "cancel" and counted.
  let asked = answer_dialog(false, None, 40).await;
  let stray = strays(&dir, &format!("frame-{nonce}"));
  check(
    &format!(
      "downloads: a cross-origin iframe opens no save dialog (frames \
       loaded {framed}, a dialog asked {asked}, strays {stray:?})"
    ),
    framed && !asked && stray.is_empty(),
  );

  // --- 4b. A download link to another origin in the top frame. ------------
  // A download never bypasses the navigation guard: `<a download>` to another
  // origin (and a plain navigation to its attachment) is not saved in-app;
  // the guard refuses it or hands it to the OS browser (captured here).
  let capture = laufey::test_capture_external_opens(true);
  let _ = laufey::test_take_external_opens();
  let foreign = format!("{other}/attach.bin?link");
  let foreign_nav = format!("{other}/attach.bin?nav");
  eval_string(
    &win,
    &format!(
      "(function() {{ const a = document.createElement('a'); \
       a.href = '{foreign}'; a.download = 'x.bin'; \
       document.body.appendChild(a); a.click(); return 'ok'; }})()"
    ),
  )
  .await;
  let asked_link = answer_dialog(false, None, 30).await;
  eval_string(&win, &format!("location.href = '{foreign_nav}'; 'ok'")).await;
  let asked_nav = answer_dialog(false, None, 30).await;
  let opened = laufey::test_take_external_opens().unwrap_or_default();
  let stayed = eval_string(&win, "String(location.origin)").await;
  let stray = strays(&dir, &format!("laufey-dl-evil-{nonce}"));
  check(
    &format!(
      "downloads: a download link or attachment on another origin opens no \
       save dialog and leaves the page (dialog {asked_link}/{asked_nav}, \
       to the browser {opened:?}, page {stayed:?}, strays {stray:?})"
    ),
    !asked_link
      && !asked_nav
      && stayed.as_deref() == Some(own.as_str())
      && stray.is_empty(),
  );
  if capture {
    check(
      &format!(
        "downloads: the top-level navigation to another origin went to the \
         browser ({opened:?})"
      ),
      opened.iter().any(|u| u == &foreign_nav),
    );
    laufey::test_capture_external_opens(false);
  } else {
    na("downloads: open-external capture (no navigation guard)");
  }

  // --- 5. A page looping downloads. ----------------------------------------
  tokio::time::sleep(BUDGET_RESET).await;
  eval_string(
    &win,
    &format!(
      "for (let i = 0; i < 8; i++) setTimeout(() => {{ {} }}, i * 50); 'ok'",
      blob_export_js(&format!("spam-{nonce}.txt"), "spam")
    ),
  )
  .await;
  // Each dialog that opens is cancelled and counted once: a cancelled
  // dialog's slot stays taken until its panel has closed (a macOS sheet
  // closes asynchronously), so after a hit the count waits for that before
  // looking again.
  let mut dialogs = 0;
  let deadline = std::time::Instant::now() + Duration::from_secs(8);
  while std::time::Instant::now() < deadline {
    if laufey::test_file_dialog_respond(false, None) {
      dialogs += 1;
      tokio::time::sleep(Duration::from_millis(1500)).await;
    } else {
      tokio::time::sleep(Duration::from_millis(100)).await;
    }
  }
  let stray = strays(&dir, &format!("spam-{nonce}"));
  check(
    &format!(
      "downloads: 8 downloads in a loop open at most 3 dialogs ({dialogs}, \
       strays {stray:?})"
    ),
    (1..=3).contains(&dialogs) && stray.is_empty(),
  );

  // --- 6. A large file. ----------------------------------------------------
  tokio::time::sleep(BUDGET_RESET).await;
  let big_out = dir.join("big-out.bin");
  eval_string(&win, "location.href = '/big.bin'; 'ok'").await;
  let asked = answer_dialog(true, Some(&big_out), 100).await;
  let want: Vec<u8> = (0..BIG_LEN).map(big_byte).collect();
  let written = asked && wait_file(&big_out, &want, 90).await;
  saved_check(
    &format!(
      "downloads: a {} MiB attachment arrives whole (asked {asked}, size \
       {:?})",
      BIG_LEN / (1024 * 1024),
      std::fs::metadata(&big_out).map(|m| m.len()).ok()
    ),
    can_accept,
    asked,
    written,
  );

  win.close();
  let _ = std::fs::remove_dir_all(&dir);
}
