// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The OS asks the app to quit (API 52): macOS's terminate: (Cmd+Q, the Dock's
// Quit, a quit Apple Event, a logout), Windows's WM_QUERYENDSESSION. The
// runtime is asked (set_quit_requested_handler) and answers
// (answer_quit_request, or quit() itself); nothing it does or fails to do can
// hold the OS past a deadline. See docs/c-abi.md ("Quit requests").

#ifndef LAUFEY_QUIT_REQUEST_H_
#define LAUFEY_QUIT_REQUEST_H_

#include <cstdint>
#include <functional>

#include "laufey.h"

namespace laufey_common {

// How long an unanswered request holds the OS before the quit goes ahead. A
// plain quit is answered at once by a live runtime, so this only bounds a
// stuck one. A session end may be held by the app while it asks its pages.
// Windows's limit stays under the 5 s after which it shows the user a
// "this app is preventing shutdown" screen.
constexpr int kQuitRequestAppDeadlineMs = 5000;
constexpr int kQuitRequestSessionEndDeadlineMacMs = 10000;
constexpr int kQuitRequestSessionEndDeadlineWinMs = 4000;

// A null handler clears it, and a request in progress goes ahead. Any thread.
void SetQuitRequestedHandler(laufey_quit_requested_fn handler, void* user_data);

enum class QuitRequestStart {
  kNoHandler,  // Nobody to ask (or the app is already quitting): go ahead.
  kAsked,      // The handler was called; `reply` runs with the answer.
  kPending,    // An earlier request is still unanswered; `reply` is dropped.
};

// Starts a request on the UI thread. `reply` runs exactly once, on whichever
// thread answers. `generation`, when given, names this request for
// ExpireQuitRequest.
QuitRequestStart BeginQuitRequest(int32_t kind,
                                  std::function<void(bool allow)> reply,
                                  uint64_t* generation = nullptr);

// Answers the request in progress: false when there is none. Any thread.
bool AnswerQuitRequest(bool allow);

// The deadline of request `generation` passed: it goes ahead, unless it was
// answered already.
void ExpireQuitRequest(uint64_t generation);

bool QuitRequestPending();

#if defined(__APPLE__)
// What the current Apple Event says: LAUFEY_QUIT_REQUEST_SESSION_END for a
// logout, restart or shutdown, else LAUFEY_QUIT_REQUEST_APP. Main thread.
int32_t QuitRequestKindMac();
// Asks about the quit in progress and starts its deadline; `reply` runs on
// the main thread, with `true` when the deadline passes. False when there is
// nothing to wait for, so the caller goes ahead at once. `pending` tells an
// earlier unanswered request from that.
bool AskQuitRequestMac(std::function<void(bool allow)> reply, bool* pending);
#endif

#if defined(_WIN32)
// The hidden top-level window that receives WM_QUERYENDSESSION and
// WM_ENDSESSION, whichever web engine owns the app's windows and with no
// window open at all (a tray app). Created once, on the UI thread; `quit` is
// the backend's quit, run when the session really ends.
void InstallSessionEndWatcherWin(void (*quit)());
#endif

}  // namespace laufey_common

#endif  // LAUFEY_QUIT_REQUEST_H_
