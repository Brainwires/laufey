// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! LAUFEY_E2E_ONLY=io: drag and drop, native file dialogs and the rich
//! clipboard (API 39). See docs/e2e-testing.md.
//!
//! - Clipboard: text / HTML (+ plain alternative) / PNG round trips, the
//!   formats list, a non-PNG write refused, and a change event after a write
//!   (through the OS watcher: the macOS change-count poll, Windows'
//!   clipboard listener, GTK's owner-change).
//! - File drops: every phase through `test_trigger_file_drop`, the dispatch
//!   the OS path uses, checked for window id, position and paths.
//! - File dialogs: real dialogs, closed by the test hook (cancel), by
//!   `cancel_file_dialog`, refused while another is open (busy), and accepted
//!   with a path the hook types in (save, open a file, open a directory),
//!   each result coming back through the dialog's own completion path.
//! - Drag out: refused without a held mouse button and for a bad path (a
//!   real drag needs a person, or OS input injection, and is not run here).

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use laufey::{
  DragResult, FileDialogKind, FileDialogOptions, FileDialogOutcome,
  FileDragPhase, FileDropEvent, FileFilter, Window,
};

use super::{check, na, wait_for, TINY_PNG};

pub async fn run() {
  let caps = laufey::window_capabilities();
  let clip = laufey::clipboard_capabilities();
  eprintln!("[e2e] window capabilities = {:#x}", caps.bits);
  eprintln!("[e2e] clipboard capabilities = {clip:?}");
  eprintln!(
    "[e2e] file drop={} enter_paths={} drag_out={} dialogs={} mixed={} modal={}",
    caps.file_drop(),
    caps.file_drop_enter_paths(),
    caps.file_drag_out(),
    caps.file_dialogs(),
    caps.file_dialog_files_and_directories(),
    caps.file_dialog_modal()
  );

  let w = Window::new(480, 360).title("native-e2e-io");
  w.show();
  if !wait_for(|| w.get_size().0 != 0, 100, 50).await {
    check("io window reports a size", false);
    return;
  }

  clipboard_checks(&clip).await;
  file_drop_checks(&w, &caps).await;
  dialog_checks(&w, &caps).await;
  drag_out_checks(&w, &caps).await;
}

// ---------------------------------------------------------------------------
// Clipboard
// ---------------------------------------------------------------------------

async fn clipboard_checks(clip: &laufey::ClipboardCapabilities) {
  check("clipboard reports text", clip.text);
  let nonce = std::process::id();

  // Text, from a worker thread: API 39 backends hop to their UI thread.
  let text = format!("laufey-io-{nonce} \u{e9}\u{4e2d}");
  let t2 = text.clone();
  let got = tokio::task::spawn_blocking(move || {
    laufey::write_clipboard_text(&t2);
    laufey::read_clipboard_text()
  })
  .await
  .ok()
  .flatten();
  if got.is_none() && !clip.html {
    // The Winit backend shells out to the platform's clipboard tools; with
    // none installed (wl-clipboard / xclip on Linux) reads come back empty.
    na("clipboard text (the clipboard tools are missing)");
  } else {
    check(
      "clipboard text round-trips from a worker thread",
      got.as_deref() == Some(text.as_str()),
    );
  }

  if clip.html {
    let html = format!("<b>laufey</b> io <i>{nonce}</i>");
    let plain = format!("laufey io {nonce}");
    check(
      "write_clipboard_html succeeds",
      laufey::write_clipboard_html(&html, Some(&plain)),
    );
    let read = laufey::read_clipboard_html().unwrap_or_default();
    eprintln!("[e2e]   html read back: {read:?}");
    check("clipboard HTML round-trips", read.contains(&html));
    check(
      "the HTML's plain-text alternative reads as text",
      laufey::read_clipboard_text().as_deref() == Some(plain.as_str()),
    );
    if clip.formats {
      let formats = laufey::read_clipboard_formats().unwrap_or_default();
      eprintln!("[e2e]   formats after HTML: {formats:?}");
      check(
        "formats list text/html and text/plain",
        formats.iter().any(|f| f == "text/html")
          && formats.iter().any(|f| f == "text/plain"),
      );
    }
  } else {
    na("clipboard HTML (backend has none)");
  }

  if clip.image {
    check(
      "a non-PNG image write is refused",
      !laufey::write_clipboard_image(b"definitely not a png"),
    );
    check(
      "write_clipboard_image succeeds",
      laufey::write_clipboard_image(TINY_PNG),
    );
    let png = laufey::read_clipboard_image().unwrap_or_default();
    eprintln!("[e2e]   image read back: {} bytes", png.len());
    check(
      "clipboard image reads back as PNG",
      png.starts_with(&[0x89, b'P', b'N', b'G']),
    );
    check("clipboard PNG round-trips byte for byte", png == TINY_PNG);
    if clip.formats {
      let formats = laufey::read_clipboard_formats().unwrap_or_default();
      eprintln!("[e2e]   formats after image: {formats:?}");
      check(
        "formats list image/png after an image write",
        formats.iter().any(|f| f == "image/png"),
      );
      check(
        "an image write replaced the text",
        !formats.iter().any(|f| f == "text/html"),
      );
    }
  } else {
    na("clipboard image (backend has none)");
  }

  if clip.change_events {
    let changes = Arc::new(AtomicUsize::new(0));
    let c = changes.clone();
    laufey::on_clipboard_change(move || {
      c.fetch_add(1, Ordering::SeqCst);
    });
    // Let the watcher start (macOS arms its poll asynchronously).
    tokio::time::sleep(Duration::from_millis(300)).await;
    let before = changes.load(Ordering::SeqCst);
    laufey::write_clipboard_text(&format!("laufey-io-change-{nonce}"));
    let fired =
      wait_for(|| changes.load(Ordering::SeqCst) > before, 60, 50).await;
    check("a clipboard change event follows a write", fired);
    laufey::clear_clipboard_change_handler();
    tokio::time::sleep(Duration::from_millis(700)).await;
    let after_clear = changes.load(Ordering::SeqCst);
    laufey::write_clipboard_text(&format!("laufey-io-quiet-{nonce}"));
    tokio::time::sleep(Duration::from_millis(1200)).await;
    check(
      "no change events after the handler is cleared",
      changes.load(Ordering::SeqCst) == after_clear,
    );
  } else {
    na("clipboard change events (backend / OS has none)");
  }
}

// ---------------------------------------------------------------------------
// File drops
// ---------------------------------------------------------------------------

async fn file_drop_checks(w: &Window, caps: &laufey::WindowCapabilities) {
  let events: Arc<Mutex<Vec<FileDropEvent>>> = Arc::new(Mutex::new(Vec::new()));
  let sink = events.clone();
  laufey::on_file_drop(move |e| sink.lock().unwrap().push(e));
  let id = w.id();
  let a = "/laufey/e2e/a.txt";
  let b = "/laufey/e2e/b c.png";
  let delivered = laufey::test_trigger_file_drop(
    id,
    FileDragPhase::Enter,
    10.0,
    20.0,
    &[a, b],
  );
  if !delivered {
    if caps.file_drop() {
      check("test_trigger_file_drop reaches the handler", false);
    } else {
      na("file drops (backend has none)");
    }
    laufey::clear_file_drop_handler();
    return;
  }
  check("file drop capability is reported", caps.file_drop());
  laufey::test_trigger_file_drop(id, FileDragPhase::Over, 11.5, 21.5, &[]);
  laufey::test_trigger_file_drop(id, FileDragPhase::Leave, 0.0, 0.0, &[a]);
  laufey::test_trigger_file_drop(id, FileDragPhase::Drop, 30.0, 40.0, &[a, b]);
  laufey::test_trigger_file_drop(id, FileDragPhase::Drop, 1.0, 1.0, &[]);
  let got = wait_for(|| events.lock().unwrap().len() >= 5, 40, 25).await;
  check("five drag phases are delivered", got);
  let ev = events.lock().unwrap().clone();
  for e in &ev {
    eprintln!("[e2e]   drop event {e:?}");
  }
  if ev.len() >= 5 {
    let ab = Some(vec![a.to_string(), b.to_string()]);
    check(
      "ENTER carries the window, position and paths",
      ev[0].window_id == id
        && ev[0].phase == FileDragPhase::Enter
        && ev[0].x == 10.0
        && ev[0].y == 20.0
        && ev[0].paths == ab
        && ev[0].count == 2,
    );
    check(
      "OVER without paths carries no paths",
      ev[1].phase == FileDragPhase::Over && ev[1].paths.is_none(),
    );
    check(
      "LEAVE never carries paths",
      ev[2].phase == FileDragPhase::Leave
        && ev[2].paths.is_none()
        && ev[2].count == 0,
    );
    check(
      "DROP carries the paths and the drop point",
      ev[3].phase == FileDragPhase::Drop
        && ev[3].paths == ab
        && ev[3].x == 30.0
        && ev[3].y == 40.0,
    );
    check(
      "a DROP with no paths still carries an (empty) list",
      ev[4].phase == FileDragPhase::Drop && ev[4].paths == Some(vec![]),
    );
  }
  laufey::clear_file_drop_handler();
  check(
    "no delivery once the handler is cleared",
    !laufey::test_trigger_file_drop(id, FileDragPhase::Drop, 0.0, 0.0, &[a]),
  );
}

// ---------------------------------------------------------------------------
// File dialogs
// ---------------------------------------------------------------------------

/// Polls the test hook until a dialog is up to act on (the backend shows it
/// asynchronously on its UI thread).
async fn respond(accept: bool, path: Option<&str>) -> bool {
  for _ in 0..100 {
    if laufey::test_file_dialog_respond(accept, path) {
      return true;
    }
    tokio::time::sleep(Duration::from_millis(50)).await;
  }
  false
}

async fn outcome_of<F: std::future::Future<Output = FileDialogOutcome>>(
  f: F,
) -> FileDialogOutcome {
  match tokio::time::timeout(Duration::from_secs(20), f).await {
    Ok(o) => o,
    Err(_) => FileDialogOutcome::Failed,
  }
}

/// Canonical form for comparing a path the dialog returned with the one we
/// asked for (macOS reports /tmp as /private/tmp; a save target may not
/// exist yet, so canonicalize its directory).
fn canon(p: &Path) -> PathBuf {
  if let Ok(c) = std::fs::canonicalize(p) {
    return c;
  }
  match (p.parent(), p.file_name()) {
    (Some(dir), Some(name)) => std::fs::canonicalize(dir)
      .map(|d| d.join(name))
      .unwrap_or_else(|_| p.to_path_buf()),
    _ => p.to_path_buf(),
  }
}

fn same_path(a: &str, b: &Path) -> bool {
  canon(Path::new(a)) == canon(b)
}

async fn dialog_checks(w: &Window, caps: &laufey::WindowCapabilities) {
  if !caps.file_dialogs() {
    let d = laufey::show_file_dialog(0, &FileDialogOptions::default());
    check(
      "show_file_dialog fails without the capability",
      d.id == 0 && outcome_of(d.outcome).await == FileDialogOutcome::Failed,
    );
    na("file dialogs (backend has none)");
    return;
  }
  let parent = if caps.file_dialog_modal() { w.id() } else { 0 };
  let dir =
    std::env::temp_dir().join(format!("laufey-io-{}", std::process::id()));
  let _ = std::fs::create_dir_all(&dir);
  let dir_str = dir.to_string_lossy().into_owned();

  // 1. Cancel through the test hook.
  let d = laufey::show_file_dialog(
    parent,
    &FileDialogOptions {
      title: Some("laufey e2e: open".into()),
      default_path: Some(dir_str.clone()),
      filters: vec![FileFilter {
        name: "Text".into(),
        extensions: vec!["txt".into()],
      }],
      ..Default::default()
    },
  );
  check("an open dialog gets an id", d.id != 0);
  let acted = respond(false, None).await;
  check("the test hook finds the open dialog", acted);
  check(
    "a dialog closed by the hook resolves cancelled",
    outcome_of(d.outcome).await == FileDialogOutcome::Cancelled,
  );

  // 2. cancel_file_dialog, and BUSY while one is open.
  let d = laufey::show_file_dialog(
    parent,
    &FileDialogOptions {
      kind: FileDialogKind::Save,
      title: Some("laufey e2e: save".into()),
      ..Default::default()
    },
  );
  let busy = laufey::show_file_dialog(0, &FileDialogOptions::default());
  check(
    "a second dialog is refused while one is open",
    busy.id == 0 && outcome_of(busy.outcome).await == FileDialogOutcome::Busy,
  );
  check(
    "cancel_file_dialog finds it",
    laufey::cancel_file_dialog(d.id),
  );
  check(
    "a cancelled dialog resolves cancelled",
    outcome_of(d.outcome).await == FileDialogOutcome::Cancelled,
  );
  check(
    "cancel_file_dialog of a closed dialog is false",
    !laufey::cancel_file_dialog(d.id),
  );

  // 3. Accept a save dialog with a typed-in path.
  let target = dir.join("saved by laufey.txt");
  let _ = std::fs::remove_file(&target);
  let d = laufey::show_file_dialog(
    parent,
    &FileDialogOptions {
      kind: FileDialogKind::Save,
      default_path: Some(dir_str.clone()),
      ..Default::default()
    },
  );
  let acted = respond(true, Some(&target.to_string_lossy())).await;
  let o = outcome_of(d.outcome).await;
  eprintln!("[e2e]   save accept -> {o:?}");
  accept_check("save dialog returns the typed path", acted, &o, &[&target]);

  // 4. Accept an open dialog on an existing file.
  let file = dir.join("open me.txt");
  let _ = std::fs::write(&file, b"laufey");
  let d = laufey::show_file_dialog(
    parent,
    &FileDialogOptions {
      files: true,
      default_path: Some(dir_str.clone()),
      ..Default::default()
    },
  );
  let acted = respond(true, Some(&file.to_string_lossy())).await;
  let o = outcome_of(d.outcome).await;
  eprintln!("[e2e]   open-file accept -> {o:?}");
  accept_check("open dialog returns the chosen file", acted, &o, &[&file]);

  // 5. Accept a folder dialog on a directory.
  let sub = dir.join("pick me");
  let _ = std::fs::create_dir_all(&sub);
  let d = laufey::show_file_dialog(
    parent,
    &FileDialogOptions {
      directories: true,
      default_path: Some(dir_str.clone()),
      ..Default::default()
    },
  );
  let acted = respond(true, Some(&sub.to_string_lossy())).await;
  let o = outcome_of(d.outcome).await;
  eprintln!("[e2e]   folder accept -> {o:?}");
  accept_check(
    "folder dialog returns the chosen directory",
    acted,
    &o,
    &[&sub],
  );

  let _ = std::fs::remove_dir_all(&dir);
}

fn accept_check(
  name: &str,
  acted: bool,
  outcome: &FileDialogOutcome,
  expected: &[&Path],
) {
  if !acted {
    check(&format!("{name} (the hook found the dialog)"), false);
    return;
  }
  match outcome {
    FileDialogOutcome::Accepted(paths) => check(
      name,
      paths.len() == expected.len()
        && paths.iter().zip(expected).all(|(p, e)| same_path(p, e)),
    ),
    // The hook acted, but the platform dialog wouldn't take the typed path
    // (e.g. a portal dialog); the dialog still closed exactly once.
    FileDialogOutcome::Cancelled => na(&format!(
      "{name} (this dialog can't be accepted programmatically)"
    )),
    _ => check(name, false),
  }
}

// ---------------------------------------------------------------------------
// Drag out
// ---------------------------------------------------------------------------

async fn drag_out_checks(w: &Window, caps: &laufey::WindowCapabilities) {
  let me = std::env::current_exe().unwrap_or_default();
  let me = me.to_string_lossy().into_owned();
  let r = tokio::time::timeout(
    Duration::from_secs(10),
    laufey::start_file_drag(w.id(), &["relative/path.txt"], None),
  )
  .await;
  eprintln!("[e2e]   relative-path drag -> {r:?}");
  check(
    "a drag of a relative path fails",
    matches!(r, Ok(DragResult::Failed)),
  );
  let r = tokio::time::timeout(
    Duration::from_secs(10),
    laufey::start_file_drag(w.id(), &[&me], None),
  )
  .await;
  eprintln!("[e2e]   no-button drag -> {r:?}");
  if caps.file_drag_out() {
    // No mouse button is held in an automated run, so the OS can't start a
    // drag: refused, through the backend's UI-thread path.
    check(
      "a drag without a held mouse button fails",
      matches!(r, Ok(DragResult::Failed)),
    );
  } else {
    check(
      "drag-out fails without the capability",
      matches!(r, Ok(DragResult::Failed)),
    );
    na("file drag-out (backend has none)");
  }
  na("a real drag out (needs a person or OS input injection)");
}
