// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Windows's side of laufey_quit_request.h.

#include <windows.h>

#include <atomic>
#include <memory>

#include "laufey_quit_request.h"

namespace laufey_common {

namespace {

constexpr wchar_t kWatcherClass[] = L"LaufeySessionEndWatcher";

std::atomic<void (*)()> g_quit{nullptr};

// Whether the session may end: the runtime's answer, or true at the deadline.
// WM_QUERYENDSESSION has to be answered from inside its window procedure, and
// the runtime's answer takes the UI thread's tasks to arrive (its page, too),
// so the loop goes on here for that long.
bool AskAboutSessionEnd() {
  auto answer = std::make_shared<std::atomic<int>>(-1);
  uint64_t generation = 0;
  QuitRequestStart start = BeginQuitRequest(
      LAUFEY_QUIT_REQUEST_SESSION_END,
      [answer](bool allow) { answer->store(allow ? 1 : 0); }, &generation);
  if (start != QuitRequestStart::kAsked)
    return true;
  const ULONGLONG deadline =
      GetTickCount64() + kQuitRequestSessionEndDeadlineWinMs;
  while (answer->load() < 0) {
    ULONGLONG now = GetTickCount64();
    if (now >= deadline) {
      ExpireQuitRequest(generation);
      break;
    }
    MsgWaitForMultipleObjectsEx(0, nullptr, static_cast<DWORD>(deadline - now),
                                QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    MSG msg;
    while (answer->load() < 0 && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) {
        // The app is ending anyway; the loop outside takes the quit.
        PostQuitMessage(static_cast<int>(msg.wParam));
        AnswerQuitRequest(true);
        return true;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  return answer->load() != 0;
}

LRESULT CALLBACK WatcherProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_QUERYENDSESSION:
      // A critical end does not wait for anyone.
      return (lp & ENDSESSION_CRITICAL) || AskAboutSessionEnd() ? TRUE : FALSE;
    case WM_ENDSESSION:
      if (wp) {
        // The session is ending: the OS ends the process soon after this
        // returns, so the quit starts now.
        if (void (*quit)() = g_quit.load())
          quit();
      }
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}

}  // namespace

void InstallSessionEndWatcherWin(void (*quit)()) {
  g_quit.store(quit);
  static bool installed = false;
  if (installed)
    return;
  installed = true;
  HINSTANCE instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WatcherProc;
  wc.hInstance = instance;
  wc.lpszClassName = kWatcherClass;
  RegisterClassExW(&wc);
  // A top-level window that is never shown: a message-only window gets no
  // WM_QUERYENDSESSION.
  CreateWindowExW(WS_EX_TOOLWINDOW, kWatcherClass, L"", WS_POPUP, 0, 0, 0, 0,
                  nullptr, nullptr, instance, nullptr);
}

}  // namespace laufey_common
