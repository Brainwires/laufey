// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The portable part of laufey_message_dialog.h: the options, the ids and
// the exactly-once result.

#include "laufey_message_dialog.h"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

namespace laufey_common {

namespace {

// Well-formed UTF-8 with no NUL (no overlongs, surrogates or values past
// U+10FFFF).
bool IsWellFormedUtf8(const char* s, size_t len) {
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  size_t i = 0;
  while (i < len) {
    unsigned char c = p[i];
    if (c == 0)
      return false;
    if (c < 0x80) {
      i++;
      continue;
    }
    size_t n;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
      n = 1;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      n = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      n = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (i + n >= len)
      return false;
    for (size_t k = 1; k <= n; k++) {
      unsigned char cc = p[i + k];
      if ((cc & 0xC0) != 0x80)
        return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) ||
        (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    i += n + 1;
  }
  return true;
}

// NULL copies as empty.
bool CopyText(const char* s, std::string* out) {
  if (!s) {
    out->clear();
    return true;
  }
  size_t len = strlen(s);
  if (!IsWellFormedUtf8(s, len))
    return false;
  out->assign(s, len);
  return true;
}

struct OpenDialog {
  laufey_message_dialog_result_fn callback = nullptr;
  void* user_data = nullptr;
};

std::mutex g_mutex;
std::map<uint32_t, OpenDialog>& OpenDialogs() {
  static auto* dialogs = new std::map<uint32_t, OpenDialog>();
  return *dialogs;
}
std::atomic<uint32_t> g_next_id{1};

}  // namespace

bool CopyMessageDialogOptions(const laufey_message_dialog_options_t* options,
                              MessageDialogRequest* out) {
  if (!options || !out)
    return false;
  if (options->kind != LAUFEY_MESSAGE_DIALOG_INFO &&
      options->kind != LAUFEY_MESSAGE_DIALOG_WARNING &&
      options->kind != LAUFEY_MESSAGE_DIALOG_ERROR)
    return false;
  if (!options->message || !options->message[0])
    return false;
  if (options->button_count > LAUFEY_MESSAGE_DIALOG_MAX_BUTTONS ||
      (options->button_count > 0 && !options->buttons))
    return false;
  MessageDialogRequest req;
  req.kind = options->kind;
  if (!CopyText(options->title, &req.title) ||
      !CopyText(options->message, &req.message) ||
      !CopyText(options->detail, &req.detail))
    return false;
  for (size_t i = 0; i < options->button_count; i++) {
    std::string label;
    if (!options->buttons[i] || !CopyText(options->buttons[i], &label))
      return false;
    req.buttons.push_back(std::move(label));
  }
  if (req.buttons.empty())
    req.buttons.push_back("OK");
  int32_t count = static_cast<int32_t>(req.buttons.size());
  req.default_index =
      options->default_index >= 0 && options->default_index < count
          ? options->default_index
          : 0;
  req.cancel_index = options->cancel_index >= 0 && options->cancel_index < count
                         ? options->cancel_index
                         : -1;
  *out = std::move(req);
  return true;
}

std::string GtkButtonLabel(const std::string& label) {
  std::string out;
  out.reserve(label.size());
  for (char c : label) {
    if (c == '_')
      out += '_';
    out += c;
  }
  return out;
}

uint32_t MessageDialogBegin(laufey_message_dialog_result_fn callback,
                            void* user_data) {
  std::lock_guard<std::mutex> lock(g_mutex);
  uint32_t id = g_next_id.fetch_add(1);
  while (id == 0 || OpenDialogs().count(id))
    id = g_next_id.fetch_add(1);
  OpenDialogs()[id] = OpenDialog{callback, user_data};
  return id;
}

void MessageDialogFinish(uint32_t dialog_id, int status, int32_t response) {
  OpenDialog dialog;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = OpenDialogs().find(dialog_id);
    if (it == OpenDialogs().end())
      return;
    dialog = it->second;
    OpenDialogs().erase(it);
  }
  if (!dialog.callback)
    return;
  if (status != LAUFEY_MESSAGE_DIALOG_ANSWERED || response < 0) {
    // An answer without a button reads as a failure, never as button 0.
    if (status == LAUFEY_MESSAGE_DIALOG_ANSWERED)
      status = LAUFEY_MESSAGE_DIALOG_FAILED;
    response = -1;
  }
  dialog.callback(dialog.user_data, dialog_id, status, response);
}

bool MessageDialogIsOpen(uint32_t dialog_id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return OpenDialogs().count(dialog_id) != 0;
}

size_t OpenMessageDialogCountForTesting() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return OpenDialogs().size();
}

uint32_t ShowMessageDialogCommon(
    const laufey_message_dialog_options_t* options,
    laufey_message_dialog_result_fn callback, void* user_data,
    const UiRunner& run_on_ui,
    std::function<void(uint32_t, const MessageDialogRequest&)> show) {
  if (!callback)
    return 0;
  MessageDialogRequest request;
  if (!CopyMessageDialogOptions(options, &request)) {
    callback(user_data, 0, LAUFEY_MESSAGE_DIALOG_FAILED, -1);
    return 0;
  }
  uint32_t id = MessageDialogBegin(callback, user_data);
  run_on_ui([id, request = std::move(request), show = std::move(show)] {
    show(id, request);
  });
  return id;
}

}  // namespace laufey_common
