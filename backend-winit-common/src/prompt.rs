// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! The prompt dialog (`LAUFEY_DIALOG_PROMPT`) of the winit / servo backends.
//!
//! The title, message and default value may come from page content, so none
//! of them is ever part of a command line a shell parses or of a script's
//! source:
//! - Windows: an in-process Win32 dialog (an in-memory template); each string
//!   is a control's text.
//! - macOS: `osascript` runs a fixed script; the strings are its `argv`
//!   (after `--`, so one starting with `-` is not an option).
//! - Linux: the first provider this session has, in order (`mod linux`):
//!   `kdialog --inputbox` (Plasma's own dialog), `zenity --entry` (GNOME's),
//!   then an in-process GTK dialog. xdg-desktop-portal has no text-input
//!   portal, so there is no portal step. The tools run directly (never
//!   through a shell), each string an `--opt=value` argument or after `--`,
//!   never a separate argument that could read as an option. With none of
//!   them (no display, or neither tool nor a GTK display), the result is
//!   [`DialogOutcome::Unsupported`]: nothing was shown, which is not a cancel.
//!
//! Returns a [`DialogOutcome`].

/// What a dialog came to.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DialogOutcome {
  /// OK; a prompt's text.
  Confirmed(Option<String>),
  /// The user dismissed it.
  Cancelled,
  /// No way to show it here; nothing was shown.
  Unsupported,
}

#[cfg(target_os = "macos")]
pub(crate) fn show_prompt_dialog(
  title: &str,
  message: &str,
  default_value: &str,
) -> DialogOutcome {
  match std::process::Command::new("osascript")
    .args(osascript_prompt_args(title, message, default_value))
    .output()
  {
    Ok(output) if output.status.success() => {
      let text = String::from_utf8_lossy(&output.stdout);
      DialogOutcome::Confirmed(Some(strip_one_newline(&text).to_string()))
    }
    _ => DialogOutcome::Cancelled,
  }
}

/// `osascript` arguments: a fixed script, then the strings as `argv`.
#[cfg(any(target_os = "macos", test))]
fn osascript_prompt_args(
  title: &str,
  message: &str,
  default_value: &str,
) -> Vec<String> {
  [
    "-e",
    "on run argv",
    "-e",
    "set r to display dialog (item 1 of argv) default answer (item 2 of argv) \
     with title (item 3 of argv) buttons {\"Cancel\", \"OK\"} default button \
     \"OK\"",
    "-e",
    "return text returned of r",
    "-e",
    "end run",
    "--",
    message,
    default_value,
    title,
  ]
  .iter()
  .map(|s| s.to_string())
  .collect()
}

/// The tool's output ends with one newline that isn't the user's.
#[cfg(any(target_os = "macos", target_os = "linux", test))]
fn strip_one_newline(s: &str) -> &str {
  s.strip_suffix('\n').unwrap_or(s)
}

#[cfg(target_os = "linux")]
pub(crate) use linux::show_prompt_dialog;

#[cfg(any(target_os = "linux", test))]
mod linux {
  use super::{strip_one_newline, DialogOutcome};

  /// An external prompt tool.
  #[derive(Debug, Clone, Copy, PartialEq, Eq)]
  pub(super) enum Tool {
    Kdialog,
    Zenity,
  }

  impl Tool {
    pub(super) fn program(self) -> &'static str {
      match self {
        Tool::Kdialog => "kdialog",
        Tool::Zenity => "zenity",
      }
    }

    pub(super) fn args(
      self,
      title: &str,
      message: &str,
      default_value: &str,
    ) -> Vec<String> {
      match self {
        Tool::Kdialog => kdialog_prompt_args(title, message, default_value),
        Tool::Zenity => zenity_prompt_args(title, message, default_value),
      }
    }
  }

  /// The order the tools are tried in.
  pub(super) const TOOLS: [Tool; 2] = [Tool::Kdialog, Tool::Zenity];

  /// What running a tool came to.
  #[derive(Debug, Clone, PartialEq, Eq)]
  pub(super) enum Run {
    /// It isn't installed (or couldn't start): try the next provider.
    Missing,
    /// It ran: its exit code (None when a signal ended it) and stdout.
    Exited(Option<i32>, String),
  }

  /// Try each tool, then the in-process dialog. `run` starts a tool (argv,
  /// never a shell); `in_process` shows the GTK dialog, or `None` when GTK
  /// has no display. Exit 0 is OK, 1 is the user cancelling; anything else
  /// (a crash, no display) moves on to the next provider.
  pub(super) fn prompt_with(
    title: &str,
    message: &str,
    default_value: &str,
    have_display: bool,
    run: impl Fn(Tool, &[String]) -> Run,
    in_process: impl FnOnce() -> Option<DialogOutcome>,
  ) -> DialogOutcome {
    if !have_display {
      return DialogOutcome::Unsupported;
    }
    for tool in TOOLS {
      match run(tool, &tool.args(title, message, default_value)) {
        Run::Missing => continue,
        Run::Exited(Some(0), out) => {
          return DialogOutcome::Confirmed(Some(
            strip_one_newline(&out).to_string(),
          ));
        }
        Run::Exited(Some(1), _) => return DialogOutcome::Cancelled,
        Run::Exited(..) => continue,
      }
    }
    in_process().unwrap_or(DialogOutcome::Unsupported)
  }

  /// `kdialog --inputbox` arguments: `--title=` and `--inputbox=` carry
  /// their values in the same argument, and the default text follows `--`,
  /// so no string can read as an option.
  pub(super) fn kdialog_prompt_args(
    title: &str,
    message: &str,
    default_value: &str,
  ) -> Vec<String> {
    vec![
      format!("--title={title}"),
      format!("--inputbox={message}"),
      "--".to_string(),
      default_value.to_string(),
    ]
  }

  /// `zenity --entry` arguments. zenity shows `--text` with
  /// `gtk_label_set_text_with_mnemonic(g_strcompress(text))`, so a literal
  /// backslash is written `\\` and a literal underscore `__`; the title and
  /// the entry text are shown as given.
  pub(super) fn zenity_prompt_args(
    title: &str,
    message: &str,
    default_value: &str,
  ) -> Vec<String> {
    let text = message.replace('\\', "\\\\").replace('_', "__");
    vec![
      "--entry".to_string(),
      format!("--title={title}"),
      format!("--text={text}"),
      format!("--entry-text={default_value}"),
    ]
  }

  #[cfg(target_os = "linux")]
  pub(crate) fn show_prompt_dialog(
    title: &str,
    message: &str,
    default_value: &str,
  ) -> DialogOutcome {
    let have_display = std::env::var_os("WAYLAND_DISPLAY")
      .is_some_and(|v| !v.is_empty())
      || std::env::var_os("DISPLAY").is_some_and(|v| !v.is_empty());
    prompt_with(
      title,
      message,
      default_value,
      have_display,
      |tool, args| match std::process::Command::new(tool.program())
        .args(args)
        .stdin(std::process::Stdio::null())
        .output()
      {
        Ok(output) => Run::Exited(
          output.status.code(),
          String::from_utf8_lossy(&output.stdout).into_owned(),
        ),
        Err(_) => Run::Missing,
      },
      || gtk_prompt(title, message, default_value),
    )
  }

  /// The in-process GTK dialog: a message dialog with an entry. `None` when
  /// GTK can't start (no display it can open, or GTK already running on
  /// another thread).
  #[cfg(target_os = "linux")]
  fn gtk_prompt(
    title: &str,
    message: &str,
    default_value: &str,
  ) -> Option<DialogOutcome> {
    use gtk::prelude::*;
    if gtk::init().is_err() {
      return None;
    }
    let dialog = gtk::MessageDialog::new(
      None::<&gtk::Window>,
      gtk::DialogFlags::MODAL,
      gtk::MessageType::Question,
      gtk::ButtonsType::OkCancel,
      message,
    );
    dialog.set_title(title);
    dialog.set_default_response(gtk::ResponseType::Ok);
    let entry = gtk::Entry::new();
    entry.set_text(default_value);
    entry.set_activates_default(true);
    dialog.content_area().add(&entry);
    entry.show();
    let response = dialog.run();
    let text = entry.text().to_string();
    // SAFETY: the dialog is ours and no reference to it outlives this call.
    unsafe { dialog.destroy() };
    while gtk::events_pending() {
      gtk::main_iteration();
    }
    Some(if response == gtk::ResponseType::Ok {
      DialogOutcome::Confirmed(Some(text))
    } else {
      DialogOutcome::Cancelled
    })
  }
}

#[cfg(not(any(
  target_os = "macos",
  target_os = "windows",
  target_os = "linux"
)))]
pub(crate) fn show_prompt_dialog(
  _title: &str,
  _message: &str,
  default_value: &str,
) -> DialogOutcome {
  DialogOutcome::Confirmed(Some(default_value.to_string()))
}

#[cfg(target_os = "windows")]
pub(crate) use win::show_prompt_dialog;

#[cfg(target_os = "windows")]
mod win {
  use std::ffi::c_void;

  type Hwnd = *mut c_void;

  const MESSAGE_ID: i32 = 100;
  const EDIT_ID: i32 = 101;
  const IDOK: i32 = 1;
  const IDCANCEL: i32 = 2;

  const WS_POPUP: u32 = 0x8000_0000;
  const WS_CHILD: u32 = 0x4000_0000;
  const WS_VISIBLE: u32 = 0x1000_0000;
  const WS_CAPTION: u32 = 0x00C0_0000;
  const WS_SYSMENU: u32 = 0x0008_0000;
  const WS_TABSTOP: u32 = 0x0001_0000;
  const WS_EX_CLIENTEDGE: u32 = 0x0000_0200;
  const DS_MODALFRAME: u32 = 0x80;
  const DS_SETFOREGROUND: u32 = 0x200;
  const DS_CENTER: u32 = 0x800;
  const SS_NOPREFIX: u32 = 0x80;
  const SS_EDITCONTROL: u32 = 0x2000;
  const ES_AUTOHSCROLL: u32 = 0x80;
  const BS_DEFPUSHBUTTON: u32 = 0x1;
  const WM_INITDIALOG: u32 = 0x0110;
  const WM_COMMAND: u32 = 0x0111;
  const WM_SETFONT: u32 = 0x0030;
  const EM_SETSEL: u32 = 0x00B1;
  const DEFAULT_GUI_FONT: i32 = 17;
  const DT_WORDBREAK: u32 = 0x10;
  const DT_CALCRECT: u32 = 0x400;
  const DT_NOPREFIX: u32 = 0x800;
  const DT_EDITCONTROL: u32 = 0x2000;
  const GWL_STYLE: i32 = -16;
  const GWL_EXSTYLE: i32 = -20;
  /// DWLP_MSGRESULT (0) + sizeof(LRESULT) + sizeof(DLGPROC).
  const DWLP_USER: i32 = 2 * std::mem::size_of::<usize>() as i32;

  #[repr(C)]
  struct Rect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
  }

  /// DLGTEMPLATE (2-byte packed) followed by its empty menu, class and title
  /// arrays; the whole template must be DWORD-aligned.
  #[repr(C, align(4))]
  struct Template {
    style: u32,
    ex_style: u32,
    cdit: u16,
    x: i16,
    y: i16,
    cx: i16,
    cy: i16,
    menu: u16,
    class: u16,
    title: u16,
  }

  type DlgProc = unsafe extern "system" fn(Hwnd, u32, usize, isize) -> isize;

  #[link(name = "user32")]
  extern "system" {
    fn DialogBoxIndirectParamW(
      instance: *mut c_void,
      template: *const Template,
      owner: Hwnd,
      proc_: DlgProc,
      param: isize,
    ) -> isize;
    fn EndDialog(dialog: Hwnd, result: isize) -> i32;
    fn CreateWindowExW(
      ex_style: u32,
      class: *const u16,
      name: *const u16,
      style: u32,
      x: i32,
      y: i32,
      w: i32,
      h: i32,
      parent: Hwnd,
      menu: isize,
      instance: *mut c_void,
      param: *mut c_void,
    ) -> Hwnd;
    fn SendMessageW(hwnd: Hwnd, msg: u32, wp: usize, lp: isize) -> isize;
    fn SetWindowTextW(hwnd: Hwnd, text: *const u16) -> i32;
    fn GetWindowTextW(hwnd: Hwnd, buf: *mut u16, len: i32) -> i32;
    fn GetWindowTextLengthW(hwnd: Hwnd) -> i32;
    fn GetDlgItem(dialog: Hwnd, id: i32) -> Hwnd;
    fn MoveWindow(
      hwnd: Hwnd,
      x: i32,
      y: i32,
      w: i32,
      h: i32,
      paint: i32,
    ) -> i32;
    fn SetFocus(hwnd: Hwnd) -> Hwnd;
    fn GetDpiForWindow(hwnd: Hwnd) -> u32;
    fn AdjustWindowRectExForDpi(
      rect: *mut Rect,
      style: u32,
      menu: i32,
      ex_style: u32,
      dpi: u32,
    ) -> i32;
    fn SetWindowPos(
      hwnd: Hwnd,
      after: Hwnd,
      x: i32,
      y: i32,
      w: i32,
      h: i32,
      flags: u32,
    ) -> i32;
    fn GetDC(hwnd: Hwnd) -> *mut c_void;
    fn ReleaseDC(hwnd: Hwnd, dc: *mut c_void) -> i32;
    fn DrawTextW(
      dc: *mut c_void,
      text: *const u16,
      len: i32,
      rect: *mut Rect,
      format: u32,
    ) -> i32;
    #[cfg_attr(target_pointer_width = "32", link_name = "SetWindowLongW")]
    fn SetWindowLongPtrW(hwnd: Hwnd, index: i32, value: isize) -> isize;
    #[cfg_attr(target_pointer_width = "32", link_name = "GetWindowLongW")]
    fn GetWindowLongPtrW(hwnd: Hwnd, index: i32) -> isize;
  }
  #[link(name = "gdi32")]
  extern "system" {
    fn GetStockObject(kind: i32) -> *mut c_void;
    fn SelectObject(dc: *mut c_void, object: *mut c_void) -> *mut c_void;
  }
  #[link(name = "kernel32")]
  extern "system" {
    fn GetModuleHandleW(name: *const u16) -> *mut c_void;
  }

  struct State {
    title: Vec<u16>,
    message: Vec<u16>,
    value: Vec<u16>,
    result: Option<String>,
  }

  fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
  }

  unsafe fn add_control(
    dialog: Hwnd,
    class: &str,
    text: *const u16,
    style: u32,
    ex_style: u32,
    id: i32,
    font: *mut c_void,
  ) -> Hwnd {
    let class = wide(class);
    let control = CreateWindowExW(
      ex_style,
      class.as_ptr(),
      text,
      WS_CHILD | WS_VISIBLE | style,
      0,
      0,
      0,
      0,
      dialog,
      id as isize,
      GetModuleHandleW(std::ptr::null()),
      std::ptr::null_mut(),
    );
    if !control.is_null() {
      SendMessageW(control, WM_SETFONT, font as usize, 0);
    }
    control
  }

  unsafe fn lay_out(dialog: Hwnd, state: &State) {
    let dpi = match GetDpiForWindow(dialog) {
      0 => 96,
      d => d as i32,
    };
    let px = |dip: i32| dip * dpi / 96;
    let font = GetStockObject(DEFAULT_GUI_FONT);
    let (margin, gap, button_w, button_h, edit_h, text_w) =
      (px(12), px(8), px(80), px(26), px(24), px(360));

    // The message's height wrapped to text_w; "&" is a character, not a
    // mnemonic marker (DT_NOPREFIX / SS_NOPREFIX).
    let mut text_rect = Rect {
      left: 0,
      top: 0,
      right: text_w,
      bottom: 0,
    };
    let dc = GetDC(dialog);
    let old = SelectObject(dc, font);
    DrawTextW(
      dc,
      state.message.as_ptr(),
      -1,
      &mut text_rect,
      DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX,
    );
    SelectObject(dc, old);
    ReleaseDC(dialog, dc);
    let text_h = text_rect.bottom.clamp(px(16), px(480));

    let label = add_control(
      dialog,
      "STATIC",
      state.message.as_ptr(),
      SS_NOPREFIX | SS_EDITCONTROL,
      0,
      MESSAGE_ID,
      font,
    );
    let edit = add_control(
      dialog,
      "EDIT",
      state.value.as_ptr(),
      WS_TABSTOP | ES_AUTOHSCROLL,
      WS_EX_CLIENTEDGE,
      EDIT_ID,
      font,
    );
    let ok_text = wide("OK");
    let cancel_text = wide("Cancel");
    let ok = add_control(
      dialog,
      "BUTTON",
      ok_text.as_ptr(),
      WS_TABSTOP | BS_DEFPUSHBUTTON,
      0,
      IDOK,
      font,
    );
    let cancel = add_control(
      dialog,
      "BUTTON",
      cancel_text.as_ptr(),
      WS_TABSTOP,
      0,
      IDCANCEL,
      font,
    );

    let mut y = margin;
    MoveWindow(label, margin, y, text_w, text_h, 0);
    y += text_h + gap;
    MoveWindow(edit, margin, y, text_w, edit_h, 0);
    y += edit_h + px(16);
    MoveWindow(cancel, margin + text_w - button_w, y, button_w, button_h, 0);
    MoveWindow(
      ok,
      margin + text_w - 2 * button_w - gap,
      y,
      button_w,
      button_h,
      0,
    );
    y += button_h + margin;

    let mut frame = Rect {
      left: 0,
      top: 0,
      right: margin * 2 + text_w,
      bottom: y,
    };
    AdjustWindowRectExForDpi(
      &mut frame,
      GetWindowLongPtrW(dialog, GWL_STYLE) as u32,
      0,
      GetWindowLongPtrW(dialog, GWL_EXSTYLE) as u32,
      dpi as u32,
    );
    // SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE: DS_CENTER placed it.
    SetWindowPos(
      dialog,
      std::ptr::null_mut(),
      0,
      0,
      frame.right - frame.left,
      frame.bottom - frame.top,
      0x2 | 0x4 | 0x10,
    );
  }

  unsafe extern "system" fn dialog_proc(
    dialog: Hwnd,
    msg: u32,
    wp: usize,
    lp: isize,
  ) -> isize {
    match msg {
      WM_INITDIALOG => {
        SetWindowLongPtrW(dialog, DWLP_USER, lp);
        let state = &*(lp as *const State);
        SetWindowTextW(dialog, state.title.as_ptr());
        lay_out(dialog, state);
        let edit = GetDlgItem(dialog, EDIT_ID);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        SetFocus(edit);
        0 // The focus is set.
      }
      WM_COMMAND => {
        let id = (wp & 0xFFFF) as i32;
        let state = GetWindowLongPtrW(dialog, DWLP_USER) as *mut State;
        if id == IDOK && !state.is_null() {
          let edit = GetDlgItem(dialog, EDIT_ID);
          let len = GetWindowTextLengthW(edit).max(0);
          let mut buf = vec![0u16; len as usize + 1];
          let n = GetWindowTextW(edit, buf.as_mut_ptr(), len + 1).max(0);
          (*state).result = Some(String::from_utf16_lossy(&buf[..n as usize]));
          EndDialog(dialog, IDOK as isize);
          1
        } else if id == IDCANCEL {
          EndDialog(dialog, IDCANCEL as isize);
          1
        } else {
          0
        }
      }
      _ => 0,
    }
  }

  pub(crate) fn show_prompt_dialog(
    title: &str,
    message: &str,
    default_value: &str,
  ) -> super::DialogOutcome {
    let template = Template {
      style: WS_POPUP
        | WS_CAPTION
        | WS_SYSMENU
        | DS_MODALFRAME
        | DS_SETFOREGROUND
        | DS_CENTER,
      ex_style: 0,
      cdit: 0,
      x: 0,
      y: 0,
      cx: 240,
      cy: 80,
      menu: 0,
      class: 0,
      title: 0,
    };
    let mut state = State {
      title: wide(title),
      message: wide(message),
      value: wide(default_value),
      result: None,
    };
    // SAFETY: the template and the state outlive the modal call, which
    // returns only once the dialog is destroyed.
    let result = unsafe {
      DialogBoxIndirectParamW(
        GetModuleHandleW(std::ptr::null()),
        &template,
        std::ptr::null_mut(),
        dialog_proc,
        &mut state as *mut State as isize,
      )
    };
    if result == IDOK as isize {
      super::DialogOutcome::Confirmed(Some(
        state.result.take().unwrap_or_default(),
      ))
    } else {
      super::DialogOutcome::Cancelled
    }
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  const HOSTILE: &str =
    "'; calc; ' $(touch /tmp/x) `id` \"q\" \\n \\\\ _a_ &b\nline2 \u{2019}; \u{1F600}";

  #[test]
  fn osascript_strings_are_argv_not_script() {
    let args = osascript_prompt_args("T -e x", HOSTILE, "-d");
    // The script lines are fixed; no string reaches them.
    let script: Vec<&String> = args.iter().take(8).collect();
    for line in &script {
      assert!(!line.contains("calc") && !line.contains("touch"));
    }
    assert_eq!(args[8], "--");
    assert_eq!(&args[9..], &[HOSTILE, "-d", "T -e x"]);
  }

  #[test]
  fn zenity_strings_are_option_values() {
    let args = linux::zenity_prompt_args("--help", HOSTILE, "--x");
    assert_eq!(args.len(), 4);
    assert_eq!(args[0], "--entry");
    assert_eq!(args[1], "--title=--help");
    assert_eq!(args[3], "--entry-text=--x");
    // Escaped so g_strcompress + the mnemonic parse give the text back.
    let text = args[2].strip_prefix("--text=").unwrap();
    assert_eq!(unmnemonic(&strcompress(text)), HOSTILE);
  }

  #[test]
  fn kdialog_strings_are_option_values_or_after_dashdash() {
    let args = linux::kdialog_prompt_args("--help", HOSTILE, "--x");
    assert_eq!(
      args,
      vec![
        "--title=--help".to_string(),
        format!("--inputbox={HOSTILE}"),
        "--".to_string(),
        "--x".to_string(),
      ]
    );
  }

  /// Runs `prompt_with` over scripted tool results, recording the tools tried.
  fn scripted(
    have_display: bool,
    kdialog: linux::Run,
    zenity: linux::Run,
    in_process: Option<DialogOutcome>,
  ) -> (DialogOutcome, Vec<&'static str>, bool) {
    let tried = std::cell::RefCell::new(Vec::new());
    let gtk_ran = std::cell::Cell::new(false);
    let outcome = linux::prompt_with(
      "T",
      "M",
      "D",
      have_display,
      |tool, args| {
        tried.borrow_mut().push(tool.program());
        // Never a shell: the program is the tool itself, the strings argv.
        assert!(!args.iter().any(|a| a == "-c"));
        match tool {
          linux::Tool::Kdialog => kdialog.clone(),
          linux::Tool::Zenity => zenity.clone(),
        }
      },
      || {
        gtk_ran.set(true);
        in_process
      },
    );
    (outcome, tried.into_inner(), gtk_ran.get())
  }

  #[test]
  fn linux_providers_in_order() {
    use linux::Run::{Exited, Missing};
    // kdialog first (Plasma); its answer is final, OK or cancel.
    let (o, tried, gtk) =
      scripted(true, Exited(Some(0), "typed\n".into()), Missing, None);
    assert_eq!(o, DialogOutcome::Confirmed(Some("typed".into())));
    assert_eq!(tried, ["kdialog"]);
    assert!(!gtk);
    let (o, tried, _) =
      scripted(true, Exited(Some(1), String::new()), Missing, None);
    assert_eq!(o, DialogOutcome::Cancelled);
    assert_eq!(tried, ["kdialog"]);
    // No kdialog (stock GNOME): zenity.
    let (o, tried, _) =
      scripted(true, Missing, Exited(Some(0), "z\n".into()), None);
    assert_eq!(o, DialogOutcome::Confirmed(Some("z".into())));
    assert_eq!(tried, ["kdialog", "zenity"]);
    // A tool that failed (crashed, couldn't open the display) is not a
    // cancel: the next provider runs.
    let (o, tried, gtk) = scripted(
      true,
      Exited(None, String::new()),
      Exited(Some(255), String::new()),
      Some(DialogOutcome::Confirmed(Some("g".into()))),
    );
    assert_eq!(o, DialogOutcome::Confirmed(Some("g".into())));
    assert_eq!(tried, ["kdialog", "zenity"]);
    assert!(gtk);
    // Neither tool (stock Kubuntu has no zenity; minimal sessions neither):
    // the in-process dialog.
    let (o, _, gtk) =
      scripted(true, Missing, Missing, Some(DialogOutcome::Cancelled));
    assert_eq!(o, DialogOutcome::Cancelled);
    assert!(gtk);
    // Nothing can show it: unsupported, never a fake cancel.
    let (o, _, _) = scripted(true, Missing, Missing, None);
    assert_eq!(o, DialogOutcome::Unsupported);
    let (o, tried, gtk) = scripted(
      false,
      Exited(Some(0), "x".into()),
      Missing,
      Some(DialogOutcome::Cancelled),
    );
    assert_eq!(o, DialogOutcome::Unsupported);
    assert!(tried.is_empty());
    assert!(!gtk);
  }

  #[test]
  fn strips_only_the_tools_newline() {
    assert_eq!(strip_one_newline(" a \n\n"), " a \n");
    assert_eq!(strip_one_newline("a"), "a");
  }

  /// g_strcompress for the escapes `zenity_prompt_args` can produce (`\\`)
  /// and the ones a message could contain (`\n`).
  fn strcompress(s: &str) -> String {
    let mut out = String::new();
    let mut it = s.chars();
    while let Some(c) = it.next() {
      if c != '\\' {
        out.push(c);
        continue;
      }
      match it.next() {
        Some('n') => out.push('\n'),
        Some('t') => out.push('\t'),
        Some(other) => out.push(other),
        None => {}
      }
    }
    out
  }

  /// GTK's mnemonic parse: `__` is `_`, a lone `_` marks the mnemonic.
  fn unmnemonic(s: &str) -> String {
    let mut out = String::new();
    let mut it = s.chars().peekable();
    while let Some(c) = it.next() {
      if c == '_' {
        if it.peek() == Some(&'_') {
          it.next();
          out.push('_');
        }
        continue;
      }
      out.push(c);
    }
    out
  }
}
