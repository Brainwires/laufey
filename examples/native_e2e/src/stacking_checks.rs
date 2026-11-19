// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The stacking order of the app's windows and the pointer (API 60), part of
//! the window-api checks (`--window-api`):
//!
//! - `window_z_order` lists the windows that are on screen, front to back:
//!   the window focused last comes first, a hidden or minimized one is not
//!   listed, and it comes back when shown again;
//! - `pointer_state` reports a point on a display, and the app's window
//!   under it: a window moved under the pointer and raised is named, and no
//!   longer once it is hidden.
//!
//! Where the backend reports neither capability (a Wayland session, Winit)
//! both calls answer `None`, which is checked instead.

use laufey::Window;

use crate::{check, na, wait_for, wm_na};

fn position(order: &[u32], id: u32) -> Option<usize> {
  order.iter().position(|w| *w == id)
}

fn order() -> Vec<u32> {
  laufey::window_z_order().unwrap_or_default()
}

/// Whether `(x, y)` is inside the window's outer frame.
const STACKING_B: &str = "laufey e2e stacking b";

/// Windows: a point of `win` (titled `title`) that no other window covers
/// (WindowFromPoint), trying it where it is and then in each corner of the
/// primary screen; Err names what covers its centre when no point of it is
/// free anywhere.
#[cfg(windows)]
async fn windows_free_point(
  win: &Window,
  title: &str,
) -> Result<(i32, i32), String> {
  use crate::io_checks::ole_input;
  let hwnd = ole_input::hwnd_of(title);
  if hwnd.is_null() {
    return Err(format!("no top-level window titled {title:?}"));
  }
  let free_here = || {
    let (x, y) = win.get_position();
    let (w, h) = win.get_outer_size();
    ole_input::free_point(hwnd, x, y, w, h)
  };
  if let Some(pt) = free_here() {
    return Ok(pt);
  }
  let (x, y) = win.get_position();
  let (w, h) = win.get_outer_size();
  let cover = ole_input::window_at(x + w / 2, y + h / 2);
  let (sw, sh) = ole_input::screen_size();
  for (cx, cy) in [
    (16, 16),
    (sw - w - 16, 16),
    (16, sh - h - 64),
    (sw - w - 16, sh - h - 64),
  ] {
    win.set_position(cx, cy);
    win.focus();
    tokio::time::sleep(std::time::Duration::from_millis(400)).await;
    if let Some(pt) = free_here() {
      eprintln!("[e2e] INFO {cover} covers the window where it was: moved to ({cx}, {cy})");
      return Ok(pt);
    }
  }
  Err(cover)
}

fn frame_contains(win: &Window, x: i32, y: i32) -> bool {
  let (wx, wy) = win.get_position();
  let (ww, wh) = win.get_outer_size();
  x >= wx && x < wx + ww && y >= wy && y < wy + wh
}

pub async fn run() {
  let caps = laufey::window_capabilities();
  if !caps.z_order() {
    check(
      "window_z_order answers None without the capability",
      laufey::window_z_order().is_none(),
    );
  }
  if !caps.pointer_state() {
    check(
      "pointer_state answers None without the capability",
      laufey::pointer_state().is_none(),
    );
  }
  if !caps.z_order() && !caps.pointer_state() {
    na("window stacking and the pointer (the session doesn't tell a client)");
    return;
  }

  let a = Window::new(320, 240).title("laufey e2e stacking a");
  let b = Window::new(320, 240).title(STACKING_B);
  a.set_position(80, 120);
  b.set_position(460, 120);
  a.show();
  b.show();

  if caps.z_order() {
    // Both on screen.
    let listed = wait_for(
      || {
        let o = order();
        position(&o, a.id()).is_some() && position(&o, b.id()).is_some()
      },
      100,
      50,
    )
    .await;
    check("window_z_order lists the windows on screen", listed);
    let o = order();
    let mut unique = o.clone();
    unique.sort_unstable();
    unique.dedup();
    check(
      "window_z_order lists each window once",
      unique.len() == o.len() && !o.contains(&0),
    );

    // The window focused last is in front of the other.
    a.focus();
    let a_front = wait_for(
      || {
        let o = order();
        matches!((position(&o, a.id()), position(&o, b.id())), (Some(x), Some(y)) if x < y)
      },
      100,
      50,
    )
    .await;
    b.focus();
    let b_front = wait_for(
      || {
        let o = order();
        matches!((position(&o, a.id()), position(&o, b.id())), (Some(x), Some(y)) if y < x)
      },
      100,
      50,
    )
    .await;
    if a_front && b_front {
      check("window_z_order follows the window raised last", true);
    } else {
      // Raising is the window manager's on Linux (none under a bare Xvfb).
      wm_na("window_z_order follows the window raised last");
    }

    // A hidden window is not stacked anywhere.
    b.hide();
    let gone = wait_for(
      || {
        let o = order();
        position(&o, b.id()).is_none() && position(&o, a.id()).is_some()
      },
      100,
      50,
    )
    .await;
    check("window_z_order leaves out a hidden window", gone);
    b.show();
    let back = wait_for(|| position(&order(), b.id()).is_some(), 100, 50).await;
    check("window_z_order lists it again once shown", back);

    // Nor is a minimized one.
    if caps.state() {
      a.minimize();
      let minimized = wait_for(|| a.is_minimized(), 100, 50).await;
      if minimized {
        let gone =
          wait_for(|| position(&order(), a.id()).is_none(), 100, 50).await;
        check("window_z_order leaves out a minimized window", gone);
        a.restore();
        let back = wait_for(
          || !a.is_minimized() && position(&order(), a.id()).is_some(),
          100,
          50,
        )
        .await;
        check("window_z_order lists it again once restored", back);
      } else {
        wm_na("window_z_order leaves out a minimized window");
      }
    }
  }

  if caps.pointer_state() {
    match laufey::pointer_state() {
      None => check("pointer_state answers with the capability", false),
      Some(p) => {
        let screens = laufey::screens();
        let on_a_display = screens.is_empty()
          || screens.iter().any(|s| {
            p.x >= s.bounds.x
              && p.x <= s.bounds.x + s.bounds.width
              && p.y >= s.bounds.y
              && p.y <= s.bounds.y + s.bounds.height
          });
        check(
          &format!("pointer_state is on a display ({}, {})", p.x, p.y),
          on_a_display,
        );
        check(
          "pointer_state reports only the three buttons",
          p.buttons
            & !(laufey::LAUFEY_POINTER_BUTTON_PRIMARY
              | laufey::LAUFEY_POINTER_BUTTON_SECONDARY
              | laufey::LAUFEY_POINTER_BUTTON_AUXILIARY)
            == 0,
        );
        // Whatever window it names holds the point.
        let named_holds_point = match p.window_id {
          None => true,
          Some(id) => frame_contains(&Window::from_id(id), p.x, p.y),
        };
        check(
          "pointer_state names a window that holds the point",
          named_holds_point,
        );

        // A window put under the pointer and raised is the one named...
        b.set_position(p.x - 100, p.y - 100);
        b.focus();
        let placed = wait_for(|| frame_contains(&b, p.x, p.y), 100, 50).await;
        // Where the pointer is checked: where it is, or, on Windows, a point
        // of the window that nothing covers, the pointer moved there
        // (SendInput). A CI image's "Microsoft account" CoreWindow sits
        // above every topmost window, over the screen centre where the
        // pointer starts; Err names what covers every point tried.
        #[cfg(windows)]
        let target: Result<(i32, i32), String> = if placed {
          windows_free_point(&b, STACKING_B).await
        } else {
          Ok((p.x, p.y))
        };
        #[cfg(not(windows))]
        let target: Result<(i32, i32), String> = Ok((p.x, p.y));
        if !placed {
          // A window manager that keeps windows on screen can refuse the
          // spot (the pointer in a corner), and a session can refuse to
          // place windows at all.
          na("pointer_state names the window under the pointer (the window could not be placed there)");
        } else if let Err(cover) = &target {
          check(
            &format!(
              "pointer_state names the window under the pointer (no point of the window is uncovered: {cover})"
            ),
            false,
          );
        } else if let Ok((tx, ty)) = target {
          #[cfg(windows)]
          if (tx, ty) != (p.x, p.y) {
            eprintln!(
              "[e2e] INFO pointer check at ({tx}, {ty}): ({}, {}) is under {}",
              p.x,
              p.y,
              crate::io_checks::ole_input::window_at(p.x, p.y)
            );
            crate::io_checks::ole_input::move_to(tx, ty);
          }
          let named = wait_for(
            || {
              laufey::pointer_state()
                .is_some_and(|q| q.window_id == Some(b.id()))
            },
            100,
            50,
          )
          .await;
          let moved =
            laufey::pointer_state().is_some_and(|q| (q.x, q.y) != (tx, ty));
          if named {
            check("pointer_state names the window under the pointer", true);
          } else if moved {
            na("pointer_state names the window under the pointer (the pointer moved during the check)");
          } else {
            let seen = laufey::pointer_state();
            eprintln!("[e2e] INFO pointer_state at ({tx}, {ty}): {seen:?}");
            check("pointer_state names the window under the pointer", false);
          }
          // ...and not once it is hidden.
          b.hide();
          let unnamed = wait_for(
            || {
              laufey::pointer_state()
                .is_some_and(|q| q.window_id != Some(b.id()))
            },
            100,
            50,
          )
          .await;
          check("pointer_state stops naming a hidden window", unnamed);
        }
      }
    }
  }

  a.close();
  b.close();
  if caps.z_order() {
    let closed = wait_for(
      || {
        let o = order();
        position(&o, a.id()).is_none() && position(&o, b.id()).is_none()
      },
      100,
      50,
    )
    .await;
    check("window_z_order leaves out closed windows", closed);
  }
}
