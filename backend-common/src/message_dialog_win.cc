// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Windows message dialogs (API 54): TaskDialogIndirect with one custom
// button per label, shown on the I/O window's thread (io_win.cc) so its
// modal loop never stalls the engine's UI thread.
//
// TaskDialogIndirect lives only in Common Controls 6. The CEF host's
// manifest asks for it; a host without that manifest (a runtime's own
// executable) has comctl32 5 loaded, which lacks the function, so it is
// looked up at run time (never imported: an import the loaded comctl32
// lacks would keep the whole backend from loading) and, when missing, an
// activation context naming Common Controls 6 is made for the call from
// shell32's own manifest. Every string is shown as text.

#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "laufey_message_dialog.h"

#include <windows.h>
#include <commctrl.h>

#include <memory>
#include <string>
#include <vector>

namespace laufey_common {

namespace {

using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*,
                                              int*, BOOL*);

// Custom button ids: kButtonBase + index (IDOK / IDCANCEL are 1 / 2).
constexpr int kButtonBase = 100;

// Common Controls 6 for the duration of a call, when the process doesn't
// already run with it. I/O thread.
class ScopedCommonControls6 {
 public:
  ScopedCommonControls6() {
    static HANDLE context = [] {
      wchar_t dir[MAX_PATH];
      UINT n = GetSystemDirectoryW(dir, MAX_PATH);
      if (n == 0 || n >= MAX_PATH)
        return INVALID_HANDLE_VALUE;
      std::wstring shell32 = std::wstring(dir) + L"\\shell32.dll";
      ACTCTXW actctx = {};
      actctx.cbSize = sizeof(actctx);
      actctx.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID;
      actctx.lpSource = shell32.c_str();
      // shell32's manifest (resource 124) depends on Common Controls 6.
      actctx.lpResourceName = MAKEINTRESOURCEW(124);
      return CreateActCtxW(&actctx);
    }();
    if (context != INVALID_HANDLE_VALUE && ActivateActCtx(context, &cookie_))
      active_ = true;
  }
  ~ScopedCommonControls6() {
    if (active_)
      DeactivateActCtx(0, cookie_);
  }
  ScopedCommonControls6(const ScopedCommonControls6&) = delete;
  ScopedCommonControls6& operator=(const ScopedCommonControls6&) = delete;

 private:
  ULONG_PTR cookie_ = 0;
  bool active_ = false;
};

// The comctl32 already loaded (the manifest's), else the one Common Controls
// 6 brings; null when neither has the function.
TaskDialogIndirectFn LookUpTaskDialog(bool* needs_context) {
  *needs_context = false;
  if (HMODULE loaded = GetModuleHandleW(L"comctl32.dll")) {
    if (auto fn = reinterpret_cast<TaskDialogIndirectFn>(
            GetProcAddress(loaded, "TaskDialogIndirect")))
      return fn;
  }
  ScopedCommonControls6 cc6;
  HMODULE v6 = LoadLibraryW(L"comctl32.dll");
  if (!v6)
    return nullptr;
  auto fn = reinterpret_cast<TaskDialogIndirectFn>(
      GetProcAddress(v6, "TaskDialogIndirect"));
  if (fn)
    *needs_context = true;
  return fn;
}

PCWSTR Icon(int kind) {
  switch (kind) {
    case LAUFEY_MESSAGE_DIALOG_WARNING:
      return TD_WARNING_ICON;
    case LAUFEY_MESSAGE_DIALOG_ERROR:
      return TD_ERROR_ICON;
    default:
      return TD_INFORMATION_ICON;
  }
}

UINT MessageBoxIcon(int kind) {
  switch (kind) {
    case LAUFEY_MESSAGE_DIALOG_WARNING:
      return MB_ICONWARNING;
    case LAUFEY_MESSAGE_DIALOG_ERROR:
      return MB_ICONERROR;
    default:
      return MB_ICONINFORMATION;
  }
}

// The window title: the request's, else the executable's name (TaskDialog
// would show the host's file name otherwise).
std::wstring Title(const MessageDialogRequest& req) {
  if (!req.title.empty())
    return Utf8ToWide(req.title);
  wchar_t path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH)
    return L"";
  std::wstring name(path, n);
  size_t slash = name.find_last_of(L"\\/");
  if (slash != std::wstring::npos)
    name = name.substr(slash + 1);
  size_t dot = name.rfind(L'.');
  if (dot != std::wstring::npos && dot > 0)
    name = name.substr(0, dot);
  return name;
}

HRESULT CALLBACK BringToFront(HWND hwnd, UINT msg, WPARAM, LPARAM, LONG_PTR) {
  if (msg == TDN_CREATED)
    SetForegroundWindow(hwnd);
  return S_OK;
}

// Runs on the I/O thread: shows the dialog (its own modal loop) and finishes
// `id` with the answer.
void ShowOnIoThread(uint32_t id, HWND owner, const MessageDialogRequest& req) {
  if (owner && !IsWindow(owner))
    owner = nullptr;
  std::wstring title = Title(req);
  std::wstring message = Utf8ToWide(req.message);
  std::wstring detail = Utf8ToWide(req.detail);
  std::vector<std::wstring> labels;
  for (const auto& b : req.buttons)
    labels.push_back(Utf8ToWide(b));

  bool needs_context = false;
  TaskDialogIndirectFn task_dialog = LookUpTaskDialog(&needs_context);
  if (!task_dialog) {
    // MessageBoxW has no custom labels: only a one-button dialog keeps its
    // meaning there (its one button is "OK").
    if (labels.size() != 1) {
      MessageDialogFinish(id, LAUFEY_MESSAGE_DIALOG_FAILED, -1);
      return;
    }
    std::wstring text = message;
    if (!detail.empty())
      text += L"\n\n" + detail;
    int ret = MessageBoxW(owner, text.c_str(), title.c_str(),
                          MB_OK | MessageBoxIcon(req.kind) |
                              (owner ? 0 : MB_TASKMODAL | MB_SETFOREGROUND));
    if (ret == 0)
      MessageDialogFinish(id, LAUFEY_MESSAGE_DIALOG_FAILED, -1);
    else
      MessageDialogFinish(id, LAUFEY_MESSAGE_DIALOG_ANSWERED, 0);
    return;
  }

  std::vector<TASKDIALOG_BUTTON> buttons;
  for (size_t i = 0; i < labels.size(); i++) {
    buttons.push_back({kButtonBase + static_cast<int>(i), labels[i].c_str()});
  }
  TASKDIALOGCONFIG config = {};
  config.cbSize = sizeof(config);
  config.hwndParent = owner;
  config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW;
  // Escape and the close button end it; with a cancel button they press it.
  config.dwFlags |= TDF_ALLOW_DIALOG_CANCELLATION;
  config.pszWindowTitle = title.c_str();
  config.pszMainIcon = Icon(req.kind);
  config.pszMainInstruction = message.c_str();
  config.pszContent = detail.empty() ? nullptr : detail.c_str();
  config.cButtons = static_cast<UINT>(buttons.size());
  config.pButtons = buttons.data();
  config.nDefaultButton = kButtonBase + req.default_index;
  if (!owner) {
    // No window to sit over (a tray app, a quit guard): come to the front.
    config.pfCallback = BringToFront;
  }

  int pressed = 0;
  HRESULT hr;
  {
    std::unique_ptr<ScopedCommonControls6> cc6;
    if (needs_context)
      cc6 = std::make_unique<ScopedCommonControls6>();
    hr = task_dialog(&config, &pressed, nullptr, nullptr);
  }
  if (FAILED(hr)) {
    MessageDialogFinish(id, LAUFEY_MESSAGE_DIALOG_FAILED, -1);
    return;
  }
  int index = pressed - kButtonBase;
  if (index >= 0 && static_cast<size_t>(index) < labels.size()) {
    MessageDialogFinish(id, LAUFEY_MESSAGE_DIALOG_ANSWERED, index);
  } else if (pressed == IDCANCEL && req.cancel_index >= 0) {
    // Escape / the close button with a cancel button: that button.
    MessageDialogFinish(id, LAUFEY_MESSAGE_DIALOG_ANSWERED, req.cancel_index);
  } else {
    MessageDialogFinish(id, LAUFEY_MESSAGE_DIALOG_CANCELLED, -1);
  }
}

}  // namespace

uint32_t ShowMessageDialogWin(ParentResolver parent,
                              const laufey_message_dialog_options_t* options,
                              laufey_message_dialog_result_fn callback,
                              void* user_data) {
  bool posted = true;
  uint32_t id = ShowMessageDialogCommon(
      options, callback, user_data,
      [&posted](std::function<void()> fn) {
        posted = WinRunOnIoThread(std::move(fn));
      },
      [parent](uint32_t dialog_id, const MessageDialogRequest& request) {
        // Resolved right before showing, as the file dialogs do; a window
        // closed meanwhile is caught by IsWindow there.
        HWND owner = parent ? static_cast<HWND>(parent()) : nullptr;
        ShowOnIoThread(dialog_id, owner, request);
      });
  if (id != 0 && !posted) {
    // No I/O thread (WinIoInit never ran): answered now, on this thread.
    MessageDialogFinish(id, LAUFEY_MESSAGE_DIALOG_FAILED, -1);
    return 0;
  }
  return id;
}

}  // namespace laufey_common
