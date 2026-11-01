// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "laufey_quit_request.h"

#include <mutex>
#include <utility>

#include "laufey_window.h"

namespace laufey_common {

namespace {

struct QuitRequestState {
  std::mutex mutex;
  laufey_quit_requested_fn handler = nullptr;
  void* user_data = nullptr;
  std::function<void(bool)> reply;
  bool pending = false;
  uint64_t generation = 0;
};

QuitRequestState& State() {
  static auto* state = new QuitRequestState();
  return *state;
}

}  // namespace

void SetQuitRequestedHandler(laufey_quit_requested_fn handler,
                             void* user_data) {
  {
    QuitRequestState& s = State();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.handler = handler;
    s.user_data = user_data;
  }
  if (!handler)
    AnswerQuitRequest(true);
}

QuitRequestStart BeginQuitRequest(int32_t kind,
                                  std::function<void(bool allow)> reply,
                                  uint64_t* generation) {
  QuitRequestState& s = State();
  laufey_quit_requested_fn handler;
  void* user_data;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.pending)
      return QuitRequestStart::kPending;
    if (!s.handler || IsQuitting())
      return QuitRequestStart::kNoHandler;
    s.pending = true;
    s.reply = std::move(reply);
    s.generation++;
    if (generation)
      *generation = s.generation;
    handler = s.handler;
    user_data = s.user_data;
  }
  handler(kind, user_data);
  return QuitRequestStart::kAsked;
}

bool AnswerQuitRequest(bool allow) {
  QuitRequestState& s = State();
  std::function<void(bool)> reply;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.pending)
      return false;
    s.pending = false;
    reply = std::move(s.reply);
    s.reply = nullptr;
  }
  if (reply)
    reply(allow);
  return true;
}

void ExpireQuitRequest(uint64_t generation) {
  QuitRequestState& s = State();
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.pending || s.generation != generation)
      return;
  }
  AnswerQuitRequest(true);
}

bool QuitRequestPending() {
  QuitRequestState& s = State();
  std::lock_guard<std::mutex> lock(s.mutex);
  return s.pending;
}

}  // namespace laufey_common
