// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Native message dialogs (API 54): the OS's own message box with the app's
// button labels, shared by the WebView and CEF backends. See
// docs/message-dialogs.md.
//
// The portable part (message_dialog.cc) copies and checks the C options,
// hands out dialog ids and delivers each result exactly once. The per-OS
// parts show the dialog on native handles: message_dialog_mac.mm (NSAlert),
// message_dialog_win.cc (TaskDialogIndirect) and message_dialog_linux.cc
// (GtkMessageDialog).

#ifndef LAUFEY_MESSAGE_DIALOG_H_
#define LAUFEY_MESSAGE_DIALOG_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "laufey.h"
#include "laufey_io.h"

namespace laufey_common {

struct MessageDialogRequest {
  int kind = LAUFEY_MESSAGE_DIALOG_INFO;
  std::string title;
  std::string message;
  std::string detail;
  // Never empty after CopyMessageDialogOptions: no buttons is one "OK".
  std::vector<std::string> buttons;
  // A valid index into `buttons`.
  int32_t default_index = 0;
  // A valid index into `buttons`, or -1 for none.
  int32_t cancel_index = -1;
};

// Copies and checks the C options. False for a NULL `options`, an unknown
// kind, a NULL or empty message, invalid UTF-8, more than
// LAUFEY_MESSAGE_DIALOG_MAX_BUTTONS buttons or a NULL label. An index out of
// range is the default (default: the first button; cancel: none).
bool CopyMessageDialogOptions(const laufey_message_dialog_options_t* options,
                              MessageDialogRequest* out);

// The label of `button` with no mnemonic underscores doubled up: GTK reads
// "_" as a mnemonic marker, so a literal one is written "__" there.
std::string GtkButtonLabel(const std::string& label);

// Reserves an id (> 0) for a dialog about to be shown; Finish delivers its
// result exactly once (later calls for the same id are ignored) and frees
// the id. `response` is used only for ANSWERED.
uint32_t MessageDialogBegin(laufey_message_dialog_result_fn callback,
                            void* user_data);
void MessageDialogFinish(uint32_t dialog_id, int status, int32_t response);
bool MessageDialogIsOpen(uint32_t dialog_id);
// How many dialogs are open (Begin'd and not yet Finish'ed). Test-only.
size_t OpenMessageDialogCountForTesting();

// Validates the request and either answers at once (FAILED, returns 0) or
// reserves an id and runs `show(id, request)` through `run_on_ui` (returns
// the id). `show` must show the platform dialog and later MessageDialogFinish
// the id, or finish it with FAILED at once.
uint32_t ShowMessageDialogCommon(
    const laufey_message_dialog_options_t* options,
    laufey_message_dialog_result_fn callback, void* user_data,
    const UiRunner& run_on_ui,
    std::function<void(uint32_t, const MessageDialogRequest&)> show);

// Each OS's ShowMessageDialog* resolves its parent window on the UI thread
// right before showing, through `parent` (null, or returning null: an
// app-modal dialog), so a window closed in between is never used.
#ifdef __APPLE__
// NSAlert: a sheet on the NSWindow* `parent` returns when it is visible and
// has no sheet already; otherwise app-modal (runModal), run from the main
// run loop rather than a main-queue block so the backend's main-queue work
// keeps running while it is up.
uint32_t ShowMessageDialogMac(ParentResolver parent,
                              const laufey_message_dialog_options_t* options,
                              laufey_message_dialog_result_fn callback,
                              void* user_data);
#endif

#ifdef _WIN32
// TaskDialogIndirect (comctl32 v6, activated for the call when the process
// has no Common Controls 6 manifest) with one custom button per label, owned
// by the HWND `parent` returns, shown on the I/O window's thread (WinIoInit).
// Without TaskDialogIndirect, a dialog of one button falls back to
// MessageBoxW; more buttons fail.
uint32_t ShowMessageDialogWin(ParentResolver parent,
                              const laufey_message_dialog_options_t* options,
                              laufey_message_dialog_result_fn callback,
                              void* user_data);
#endif

#ifdef __linux__
// GtkMessageDialog with the labels as buttons (response ids = indexes),
// transient for the GtkWindow* `parent` returns, else app-modal with no
// parent. Shown without a nested loop; quitting ends it as CANCELLED.
uint32_t ShowMessageDialogLinux(ParentResolver parent,
                                const laufey_message_dialog_options_t* options,
                                laufey_message_dialog_result_fn callback,
                                void* user_data);

// Ends every open message dialog as CANCELLED, on the GTK thread next time
// it iterates. Part of CancelGtkDialogsForQuit. Any thread.
void CancelGtkMessageDialogsForQuit();
#endif

}  // namespace laufey_common

#endif  // LAUFEY_MESSAGE_DIALOG_H_
