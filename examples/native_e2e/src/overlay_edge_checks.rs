//! Where a layer surface lands (API 52, LAUFEY_E2E_ONLY=overlay-edge,
//! `--overlay-edge`): an overlay placed 16 px in from the bottom-right corner
//! of its output, in screen coordinates, stays clear of the panels there. The
//! surface's rectangle comes from the compositor (a KWin script). GDK's
//! work area on Wayland is the whole output, so the panels' size is unknown to
//! the app; the compositor is what keeps the window clear of them.
//!
//! N/A without a layer-shell compositor, or on one that isn't KWin.

use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::Duration;

use laufey::{Window, WindowOptions};

use crate::{check, na, wait_for};

const TITLE: &str = "native-e2e-overlay-edge";

/// `qdbus6`, or what the session calls it. Output piped, stdin closed.
fn qdbus(args: &[&str]) -> Option<String> {
  for tool in ["qdbus6", "qdbus", "qdbus-qt6"] {
    let out = Command::new(tool)
      .args(args)
      .stdin(Stdio::null())
      .stderr(Stdio::null())
      .output();
    if let Ok(out) = out {
      if out.status.success() {
        return Some(String::from_utf8_lossy(&out.stdout).trim().to_string());
      }
    }
  }
  None
}

/// Runs a KWin script that prints every window; the lines come back through
/// the journal, KWin's stdout.
fn kwin_windows() -> Option<Vec<String>> {
  // Its own marker per call: the journal still holds the earlier calls' lines.
  static CALLS: AtomicU32 = AtomicU32::new(0);
  let call = CALLS.fetch_add(1, Ordering::SeqCst);
  let marker = format!("laufey-e2e-{}-{call}", std::process::id());
  let path = std::env::temp_dir().join(format!("{marker}.js"));
  std::fs::write(
    &path,
    format!(
      r#"var ws = workspace.windowList ? workspace.windowList() : workspace.clientList();
for (var i = 0; i < ws.length; i++) {{
  var w = ws[i], g = w.frameGeometry;
  print("{marker} " + JSON.stringify({{
    cls: String(w.resourceClass), name: String(w.resourceName),
    caption: String(w.caption), dock: !!w.dock, x: g.x, y: g.y, w: g.width, h: g.height
  }}));
}}"#
    ),
  )
  .ok()?;
  let id = qdbus(&[
    "org.kde.KWin",
    "/Scripting",
    "org.kde.kwin.Scripting.loadScript",
    path.to_str()?,
    &marker,
  ])?;
  let script = format!("/Scripting/Script{id}");
  qdbus(&["org.kde.KWin", &script, "org.kde.kwin.Script.run"])?;
  std::thread::sleep(Duration::from_millis(500));
  qdbus(&["org.kde.KWin", &script, "org.kde.kwin.Script.stop"]);
  let _ = std::fs::remove_file(&path);
  let out = Command::new("journalctl")
    .args(["--user", "--since", "-30s", "-o", "cat", "--no-pager"])
    .stdin(Stdio::null())
    .output()
    .ok()?;
  let text = String::from_utf8_lossy(&out.stdout).to_string();
  let lines: Vec<String> = text
    .lines()
    .filter_map(|l| l.split_once(&format!("{marker} ")))
    .map(|(_, rest)| rest.to_string())
    .collect();
  Some(lines)
}

/// One number out of the script's JSON line.
fn num(line: &str, key: &str) -> Option<i32> {
  let start = line.find(&format!("\"{key}\":"))? + key.len() + 3;
  let end = line[start..]
    .find(|c: char| !(c.is_ascii_digit() || c == '-' || c == '.'))
    .map(|n| start + n)
    .unwrap_or(line.len());
  line[start..end].parse::<f64>().ok().map(|v| v as i32)
}

pub async fn run() {
  let caps = laufey::overlay_capabilities();
  check(
    "layer-edge placement goes with layer shell",
    caps.layer_edge_anchor() == caps.layer_shell(),
  );
  if !caps.layer_shell() {
    na("layer-surface placement (no layer-shell compositor here)");
    return;
  }

  let screens = laufey::screens();
  let Some(screen) = screens.iter().find(|s| s.is_primary).or(screens.first())
  else {
    check("a screen is listed", false);
    return;
  };
  let (sx, sy, sw, sh) = (
    screen.bounds.x as i32,
    screen.bounds.y as i32,
    screen.bounds.width as i32,
    screen.bounds.height as i32,
  );
  eprintln!("[e2e] INFO output {sx},{sy} {sw}x{sh}");

  let (w, h) = (260, 300);
  let win = Window::new_with_options(
    w,
    h,
    WindowOptions {
      overlay: true,
      hidden: true,
      ..Default::default()
    },
  )
  .title(TITLE);
  let _ = wait_for(|| win.get_size().0 != 0, 100, 50).await;
  let (x, y) = (sx + sw - w - 16, sy + sh - h - 16);
  win.navigate("laufey-e2e://app/titled/overlay-edge");
  win.show();
  tokio::time::sleep(Duration::from_millis(800)).await;
  win.set_position(x, y);
  tokio::time::sleep(Duration::from_millis(1500)).await;
  check(
    "get_position answers the screen coordinates set",
    win.get_position() == (x, y),
  );
  tokio::time::sleep(Duration::from_millis(800)).await;

  let Some(lines) = kwin_windows() else {
    na("the surface's place (no KWin on this session's bus)");
    win.close();
    return;
  };
  eprintln!("[e2e] INFO {} windows from KWin", lines.len());
  for line in &lines {
    eprintln!("[e2e] INFO kwin {line}");
  }
  let surface = lines
    .iter()
    .find(|l| num(l, "w") == Some(w) && num(l, "h") == Some(h));
  let Some(surface) = surface else {
    check("KWin lists the layer surface", false);
    win.close();
    return;
  };
  let bottom = num(surface, "y").unwrap_or(0) + h;
  let right = num(surface, "x").unwrap_or(0) + w;
  // A panel at the bottom (a dock in KWin's list, at most half the output).
  let panel_top = lines
    .iter()
    .filter(|l| l.contains("\"dock\":true"))
    .filter_map(|l| {
      let (py, ph) = (num(l, "y")?, num(l, "h")?);
      (py + ph >= sy + sh && py > sy + sh / 2).then_some(py)
    })
    .min();
  eprintln!(
    "[e2e] INFO surface bottom {bottom}, right {right}, panel top {panel_top:?}"
  );
  check(
    "the surface's bottom is above the bottom panel's top (or the output's)",
    bottom <= panel_top.unwrap_or(sy + sh) - 16,
  );
  check(
    "the surface is 16 px from the output's right edge",
    right == sx + sw - 16,
  );

  // The top-left quadrant stays top-left, at the coordinates given.
  let (x2, y2) = (sx + 100, sy + 120);
  win.set_position(x2, y2);
  tokio::time::sleep(Duration::from_millis(1000)).await;
  if let Some(lines) = kwin_windows() {
    let s = lines
      .iter()
      .find(|l| num(l, "w") == Some(w) && num(l, "h") == Some(h));
    check(
      "the top-left quadrant anchors top-left (a panel on top may push it down)",
      s.is_some_and(|l| {
        num(l, "x") == Some(x2) && num(l, "y").is_some_and(|v| v >= y2)
      }),
    );
  }
  win.close();
}
