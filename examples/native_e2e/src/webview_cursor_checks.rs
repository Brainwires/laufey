//! The cursor over a child web view (API 58): part of the window-api battery.
//!
//! macOS (WKWebView): a child web view is a subview of the window's page
//! view, and the page view's WebKit tracking area covers it. Before the fix
//! the window's page heard every mouse move over the child, worked out the
//! cursor of its own element under the child and set it, racing the child
//! page's cursor (a flicker). Here the window's page puts an element with
//! `cursor: pointer` under a child whose page is all `cursor: text`, and
//! mouse moves (in-process NSEvents, with the real cursor warped to each
//! point, so WebKit sees the window under the mouse) go:
//!
//! - over the page's element beside the child: the page's pointer (the
//!   control: without it the moves don't reach WebKit and the rest is N/A);
//! - over the child, N times: the child's I-beam after every move;
//! - over the child once it is hidden: the page's pointer again (a hidden
//!   view takes no moves and sets no cursor).
//!
//! The cursor is `+[NSCursor currentCursor]`, read on the main thread. Other
//! platforms and backends: N/A (WebKitGTK views have their own GdkWindow and
//! Chromium (CEF, WebView2) sets a cursor only for its window under the
//! pointer; see docs/webviews.md).

// The checks run on macOS only; elsewhere the module just says N/A.
#![cfg_attr(not(target_os = "macos"), allow(dead_code, unused_imports))]

use std::io::{BufRead, BufReader, Write};
use std::net::TcpListener;
use std::time::Duration;

use laufey::{WebViewBounds, WebViewEventKind, WebViewOptions, Window};

use crate::{check, na};

const CHILD_PAGE: &str = "<!doctype html><title>cursor-child</title>\
<style>html,body{margin:0;height:100%;cursor:text;background:#eef}</style>\
<body></body>";

/// The child, in the window's content (top-left DIPs).
const CHILD: (f64, f64, f64, f64) = (40.0, 40.0, 200.0, 120.0);
/// The page's `cursor: pointer` element: the child's area and more to its
/// right.
const UNDER: (f64, f64, f64, f64) = (40.0, 40.0, 320.0, 200.0);
const MOVES: usize = 40;

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
        loop {
          let mut h = String::new();
          match reader.read_line(&mut h) {
            Ok(0) | Err(_) => break,
            Ok(_) if h == "\r\n" || h == "\n" => break,
            Ok(_) => {}
          }
        }
        let response = format!(
          "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{CHILD_PAGE}",
          CHILD_PAGE.len()
        );
        let mut out = stream;
        let _ = out.write_all(response.as_bytes());
      });
    }
  });
  Some(format!("http://127.0.0.1:{port}"))
}

pub async fn run(w: &Window, title: &str) {
  let backend = std::env::var("LAUFEY_E2E_BACKEND").unwrap_or_default();
  if !cfg!(target_os = "macos") || backend != "webview" {
    na("the cursor over a child web view (macOS WKWebView only)");
    return;
  }
  #[cfg(target_os = "macos")]
  mac_run(w, title).await;
  #[cfg(not(target_os = "macos"))]
  let _ = (w, title);
}

#[cfg(target_os = "macos")]
async fn mac_run(w: &Window, title: &str) {
  use mac::Cursor;

  if !laufey::webview_capabilities().supported() {
    na("the cursor over a child web view (no web views)");
    return;
  }
  let Some(base) = serve() else {
    check("cursor checks: a loopback server", false);
    return;
  };
  let (x, y, width, height) = UNDER;
  let placed = crate::page_number(
    w,
    &format!(
      "(() => {{ let e = document.getElementById('laufey-e2e-under'); \
       if (!e) {{ e = document.createElement('div'); \
       e.id = 'laufey-e2e-under'; document.documentElement.appendChild(e); }} \
       e.style.cssText = 'position:fixed;left:{x}px;top:{y}px;width:{width}px;\
       height:{height}px;cursor:pointer;z-index:2147483647;background:#fee'; \
       return 1; }})()"
    ),
  )
  .await;
  if placed != Some(1.0) {
    na("the cursor over a child web view (the page doesn't answer)");
    return;
  }

  let events = std::sync::Arc::new(std::sync::Mutex::new(Vec::new()));
  let sink = events.clone();
  let (cx, cy, cw, ch) = CHILD;
  let created = laufey::create_webview(
    w.id(),
    &WebViewOptions {
      url: format!("{base}/"),
      bounds: WebViewBounds {
        x: cx,
        y: cy,
        width: cw,
        height: ch,
      },
      partition: Some("ephemeral:e2e-cursor".into()),
      ..Default::default()
    },
    move |e| sink.lock().unwrap().push(e),
  );
  let Ok(view) = created else {
    check(
      &format!("cursor checks: a child web view ({created:?})"),
      false,
    );
    return;
  };
  let mut loaded = false;
  for _ in 0..400 {
    if events
      .lock()
      .unwrap()
      .iter()
      .any(|e: &laufey::WebViewEvent| {
        e.kind == WebViewEventKind::Load && e.detail_json.contains("complete")
      })
    {
      loaded = true;
      break;
    }
    tokio::time::sleep(Duration::from_millis(50)).await;
  }
  if !loaded {
    check("cursor checks: the child page loads", false);
    view.close();
    return;
  }

  // The window key and the app active: WebKit's tracking area hears moves
  // only in the key window.
  w.focus();
  tokio::time::sleep(Duration::from_millis(500)).await;

  // Moves to each point, waits for both pages' answers, reads the cursor.
  let sweep = |points: Vec<(f64, f64)>| {
    let t = title.to_string();
    async move {
      let mut seen = Vec::with_capacity(points.len());
      for (px, py) in points {
        let t2 = t.clone();
        let posted =
          laufey::spawn_on_ui_thread(move || mac::move_to(&t2, px, py))
            .await
            .unwrap_or(false);
        if !posted {
          return None;
        }
        tokio::time::sleep(Duration::from_millis(40)).await;
        seen.push(
          laufey::spawn_on_ui_thread(mac::current_cursor)
            .await
            .unwrap_or(Cursor::Other),
        );
      }
      Some(seen)
    }
  };
  // A zigzag inside a rectangle, never on its edge.
  let zigzag = |(rx, ry, rw, rh): (f64, f64, f64, f64), n: usize| {
    (0..n)
      .map(|i| {
        let fx = (i % 10) as f64 / 10.0;
        let fy = ((i * 7) % 10) as f64 / 10.0;
        (rx + 10.0 + fx * (rw - 20.0), ry + 10.0 + fy * (rh - 20.0))
      })
      .collect::<Vec<_>>()
  };
  // The page's element right of the child.
  let beside = (cx + cw + 10.0, y, x + width - (cx + cw + 10.0), height);

  let Some(control) = sweep(zigzag(beside, 10)).await else {
    na("the cursor over a child web view (no window to post moves to)");
    view.close();
    return;
  };
  if control.last() != Some(&Cursor::PointingHand) {
    eprintln!("[e2e]   cursor beside the child: {control:?}");
    na(
      "the cursor over a child web view (posted moves don't reach WebKit \
       here: the page never set its pointer)",
    );
    view.close();
    return;
  }
  check(
    "macOS: beside a child web view the page's own cursor shows",
    true,
  );

  let over = sweep(zigzag(CHILD, MOVES)).await.unwrap_or_default();
  // The first moves may still show the page's cursor (the child answers
  // the move into it a moment later).
  let settled: Vec<_> = over.iter().skip(3).collect();
  let others = settled.iter().filter(|c| ***c != Cursor::IBeam).count();
  if others > 0 {
    eprintln!("[e2e]   cursor over the child: {over:?}");
  }
  check(
    &format!(
      "macOS: over a visible child web view only its page sets the cursor \
       ({others} of {} moves showed another)",
      settled.len()
    ),
    !settled.is_empty() && others == 0,
  );

  // Hidden (a snapshot swap): the page's element under it is the page's.
  check("cursor checks: hide the child", view.set_visible(false));
  tokio::time::sleep(Duration::from_millis(200)).await;
  let hidden = sweep(zigzag(CHILD, 10)).await.unwrap_or_default();
  if hidden.last() != Some(&Cursor::PointingHand) {
    eprintln!("[e2e]   cursor over the hidden child: {hidden:?}");
  }
  check(
    "macOS: over a hidden child web view the page's cursor shows",
    hidden.last() == Some(&Cursor::PointingHand),
  );

  view.close();
  let _ = crate::page_number(
    w,
    "(() => { document.getElementById('laufey-e2e-under')?.remove(); \
     return 1; })()",
  )
  .await;
}

#[cfg(target_os = "macos")]
mod mac {
  //! The window by its title, real cursor warps, posted NSEvent moves and
  //! the current NSCursor through the Objective-C runtime. Main thread.
  use std::ffi::{c_char, c_void, CStr};

  #[repr(C)]
  #[derive(Clone, Copy)]
  struct CGPoint {
    x: f64,
    y: f64,
  }
  #[repr(C)]
  #[derive(Clone, Copy)]
  struct CGSize {
    width: f64,
    height: f64,
  }
  #[repr(C)]
  #[derive(Clone, Copy)]
  struct CGRect {
    origin: CGPoint,
    size: CGSize,
  }

  #[link(name = "objc")]
  extern "C" {
    fn objc_getClass(name: *const c_char) -> *mut c_void;
    fn sel_registerName(name: *const c_char) -> *mut c_void;
    fn objc_msgSend();
  }

  #[link(name = "CoreGraphics", kind = "framework")]
  extern "C" {
    fn CGMainDisplayID() -> u32;
    fn CGDisplayBounds(display: u32) -> CGRect;
    fn CGWarpMouseCursorPosition(point: CGPoint) -> i32;
  }

  const NS_EVENT_TYPE_MOUSE_MOVED: u64 = 5;

  #[derive(Debug, Clone, Copy, PartialEq, Eq)]
  pub enum Cursor {
    IBeam,
    PointingHand,
    Arrow,
    Other,
  }

  fn sel(name: &CStr) -> *mut c_void {
    unsafe { sel_registerName(name.as_ptr()) }
  }

  fn class(name: &CStr) -> *mut c_void {
    unsafe { objc_getClass(name.as_ptr()) }
  }

  unsafe fn send0(obj: *mut c_void, s: &CStr) -> *mut c_void {
    let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void =
      std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel(s))
  }

  unsafe fn send_usize(obj: *mut c_void, s: &CStr) -> usize {
    let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> usize =
      std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel(s))
  }

  unsafe fn send_bool(obj: *mut c_void, s: &CStr) -> bool {
    let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool =
      std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel(s))
  }

  unsafe fn send_f64(obj: *mut c_void, s: &CStr) -> f64 {
    let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> f64 =
      std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel(s))
  }

  unsafe fn send_rect(obj: *mut c_void, s: &CStr) -> CGRect {
    // A 32-byte struct comes back through objc_msgSend_stret on x86_64.
    #[cfg(target_arch = "x86_64")]
    {
      #[link(name = "objc")]
      extern "C" {
        fn objc_msgSend_stret();
      }
      let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> CGRect =
        std::mem::transmute(objc_msgSend_stret as unsafe extern "C" fn());
      f(obj, sel(s))
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
      let f: unsafe extern "C" fn(*mut c_void, *mut c_void) -> CGRect =
        std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
      f(obj, sel(s))
    }
  }

  unsafe fn send_point_view(
    obj: *mut c_void,
    s: &CStr,
    p: CGPoint,
    view: *mut c_void,
  ) -> CGPoint {
    let f: unsafe extern "C" fn(
      *mut c_void,
      *mut c_void,
      CGPoint,
      *mut c_void,
    ) -> CGPoint = std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel(s), p, view)
  }

  unsafe fn send_point(obj: *mut c_void, s: &CStr, p: CGPoint) -> CGPoint {
    let f: unsafe extern "C" fn(*mut c_void, *mut c_void, CGPoint) -> CGPoint =
      std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
    f(obj, sel(s), p)
  }

  fn window_titled(title: &str) -> Option<*mut c_void> {
    unsafe {
      let app = send0(class(c"NSApplication"), c"sharedApplication");
      let windows = send0(app, c"windows");
      for i in 0..send_usize(windows, c"count") {
        let f: unsafe extern "C" fn(
          *mut c_void,
          *mut c_void,
          usize,
        ) -> *mut c_void =
          std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
        let window = f(windows, sel(c"objectAtIndex:"), i);
        let name = send0(window, c"title");
        let utf8 = send0(name, c"UTF8String") as *const c_char;
        if !utf8.is_null()
          && CStr::from_ptr(utf8).to_bytes() == title.as_bytes()
        {
          return Some(window);
        }
      }
      None
    }
  }

  /// Puts the real cursor over (x, y) of the window's content (top-left
  /// DIPs) and posts a mouse move there to the app, as the window server
  /// would. False without the window.
  pub fn move_to(title: &str, x: f64, y: f64) -> bool {
    let Some(window) = window_titled(title) else {
      return false;
    };
    unsafe {
      let content = send0(window, c"contentView");
      if content.is_null() {
        return false;
      }
      let bounds = send_rect(content, c"bounds");
      let local = if send_bool(content, c"isFlipped") {
        CGPoint { x, y }
      } else {
        CGPoint {
          x,
          y: bounds.size.height - y,
        }
      };
      let in_window = send_point_view(
        content,
        c"convertPoint:toView:",
        local,
        std::ptr::null_mut(),
      );
      let on_screen = send_point(window, c"convertPointToScreen:", in_window);
      let screen_h = CGDisplayBounds(CGMainDisplayID()).size.height;
      CGWarpMouseCursorPosition(CGPoint {
        x: on_screen.x,
        y: screen_h - on_screen.y,
      });

      let info = send0(class(c"NSProcessInfo"), c"processInfo");
      let now = send_f64(info, c"systemUptime");
      let number = send_usize(window, c"windowNumber") as isize;
      let make: unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        u64,
        CGPoint,
        u64,
        f64,
        isize,
        *mut c_void,
        isize,
        isize,
        f32,
      ) -> *mut c_void =
        std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
      let event = make(
        class(c"NSEvent"),
        sel(c"mouseEventWithType:location:modifierFlags:timestamp:windowNumber:context:eventNumber:clickCount:pressure:"),
        NS_EVENT_TYPE_MOUSE_MOVED,
        in_window,
        0,
        now,
        number,
        std::ptr::null_mut(),
        0,
        0,
        0.0,
      );
      if event.is_null() {
        return false;
      }
      let app = send0(class(c"NSApplication"), c"sharedApplication");
      let post: unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        *mut c_void,
        bool,
      ) = std::mem::transmute(objc_msgSend as unsafe extern "C" fn());
      post(app, sel(c"postEvent:atStart:"), event, false);
      true
    }
  }

  /// `+[NSCursor currentCursor]`, by the system cursors WebKit uses for
  /// `text`, `pointer` and `default`.
  pub fn current_cursor() -> Cursor {
    unsafe {
      let cls = class(c"NSCursor");
      let current = send0(cls, c"currentCursor");
      if current.is_null() {
        return Cursor::Other;
      }
      if current == send0(cls, c"IBeamCursor") {
        Cursor::IBeam
      } else if current == send0(cls, c"pointingHandCursor") {
        Cursor::PointingHand
      } else if current == send0(cls, c"arrowCursor") {
        Cursor::Arrow
      } else {
        Cursor::Other
      }
    }
  }
}
