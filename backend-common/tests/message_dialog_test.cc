// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// laufey_message_dialog.h (API 54): the options a dialog is shown with, its
// id, and that its result is delivered exactly once. No OS.

#include "laufey_message_dialog.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#define EXPECT(cond)                                                         \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, \
                   #cond);                                                   \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

using namespace laufey_common;

namespace {

struct Result {
  uint32_t id;
  int status;
  int32_t response;
};

std::vector<Result> g_results;

void OnResult(void* user_data, uint32_t id, int status, int32_t response) {
  EXPECT(user_data == &g_results);
  g_results.push_back({id, status, response});
}

laufey_message_dialog_options_t Options(const char* message) {
  laufey_message_dialog_options_t o = {};
  o.kind = LAUFEY_MESSAGE_DIALOG_INFO;
  o.message = message;
  o.default_index = -1;
  o.cancel_index = -1;
  return o;
}

}  // namespace

int main() {
  // --- CopyMessageDialogOptions ------------------------------------------
  {
    const char* labels[] = {"Save", "Don't Save", "Cancel"};
    laufey_message_dialog_options_t o = Options("Save changes?");
    o.kind = LAUFEY_MESSAGE_DIALOG_WARNING;
    o.title = "Editor";
    o.detail = "Your changes will be lost.";
    o.buttons = labels;
    o.button_count = 3;
    o.default_index = 0;
    o.cancel_index = 2;
    MessageDialogRequest r;
    EXPECT(CopyMessageDialogOptions(&o, &r));
    EXPECT(r.kind == LAUFEY_MESSAGE_DIALOG_WARNING);
    EXPECT(r.title == "Editor" && r.message == "Save changes?");
    EXPECT(r.detail == "Your changes will be lost.");
    EXPECT(r.buttons.size() == 3 && r.buttons[1] == "Don't Save");
    EXPECT(r.default_index == 0 && r.cancel_index == 2);
  }
  {
    // No buttons: one "OK"; no indexes: the first is the default, no cancel.
    laufey_message_dialog_options_t o = Options("Done");
    MessageDialogRequest r;
    EXPECT(CopyMessageDialogOptions(&o, &r));
    EXPECT(r.buttons.size() == 1 && r.buttons[0] == "OK");
    EXPECT(r.default_index == 0 && r.cancel_index == -1);
    EXPECT(r.title.empty() && r.detail.empty());
  }
  {
    // Out-of-range indexes are the defaults.
    const char* labels[] = {"A", "B"};
    laufey_message_dialog_options_t o = Options("m");
    o.buttons = labels;
    o.button_count = 2;
    o.default_index = 5;
    o.cancel_index = 2;
    MessageDialogRequest r;
    EXPECT(CopyMessageDialogOptions(&o, &r));
    EXPECT(r.default_index == 0 && r.cancel_index == -1);
  }
  {
    MessageDialogRequest r;
    EXPECT(!CopyMessageDialogOptions(nullptr, &r));
    laufey_message_dialog_options_t o = Options(nullptr);
    EXPECT(!CopyMessageDialogOptions(&o, &r));  // no message
    o = Options("");
    EXPECT(!CopyMessageDialogOptions(&o, &r));  // empty message
    o = Options("m");
    o.kind = 7;
    EXPECT(!CopyMessageDialogOptions(&o, &r));  // unknown kind
    o = Options("bad \xC0\xAF utf-8");
    EXPECT(!CopyMessageDialogOptions(&o, &r));  // overlong encoding
    o = Options("m");
    o.detail = "\xED\xA0\x80";  // a surrogate
    EXPECT(!CopyMessageDialogOptions(&o, &r));
    o = Options("m");
    o.button_count = 2;  // buttons NULL
    EXPECT(!CopyMessageDialogOptions(&o, &r));
    const char* with_null[] = {"A", nullptr};
    o.buttons = with_null;
    EXPECT(!CopyMessageDialogOptions(&o, &r));
    std::vector<const char*> many(LAUFEY_MESSAGE_DIALOG_MAX_BUTTONS + 1, "b");
    o.buttons = many.data();
    o.button_count = many.size();
    EXPECT(!CopyMessageDialogOptions(&o, &r));
    o.button_count = LAUFEY_MESSAGE_DIALOG_MAX_BUTTONS;
    EXPECT(CopyMessageDialogOptions(&o, &r));
    // Non-ASCII text is kept as is.
    o = Options("\xE2\x9C\x93 Gespeichert");
    EXPECT(CopyMessageDialogOptions(&o, &r));
    EXPECT(r.message == "\xE2\x9C\x93 Gespeichert");
  }

  // --- GtkButtonLabel ----------------------------------------------------
  EXPECT(GtkButtonLabel("Save") == "Save");
  EXPECT(GtkButtonLabel("_Save as_") == "__Save as__");

  // --- Ids and the exactly-once result -----------------------------------
  {
    std::vector<std::function<void()>> queued;
    UiRunner later = [&queued](std::function<void()> fn) {
      queued.push_back(std::move(fn));
    };
    std::vector<uint32_t> shown;
    auto show = [&shown](uint32_t id, const MessageDialogRequest& req) {
      EXPECT(req.message == "Quit?");
      shown.push_back(id);
    };

    // Bad options: answered at once on this thread, nothing queued.
    laufey_message_dialog_options_t bad = Options("");
    EXPECT(ShowMessageDialogCommon(&bad, OnResult, &g_results, later, show) ==
           0);
    EXPECT(g_results.size() == 1 && g_results[0].id == 0 &&
           g_results[0].status == LAUFEY_MESSAGE_DIALOG_FAILED &&
           g_results[0].response == -1);
    EXPECT(queued.empty());
    // A NULL callback shows nothing.
    laufey_message_dialog_options_t ok = Options("Quit?");
    EXPECT(ShowMessageDialogCommon(&ok, nullptr, nullptr, later, show) == 0);
    EXPECT(queued.empty());

    // Two at once: each gets its own id, shown on the UI runner.
    uint32_t a =
        ShowMessageDialogCommon(&ok, OnResult, &g_results, later, show);
    uint32_t b =
        ShowMessageDialogCommon(&ok, OnResult, &g_results, later, show);
    EXPECT(a != 0 && b != 0 && a != b);
    EXPECT(MessageDialogIsOpen(a) && MessageDialogIsOpen(b));
    EXPECT(OpenMessageDialogCountForTesting() == 2);
    EXPECT(shown.empty() && queued.size() == 2);
    for (auto& fn : queued)
      fn();
    EXPECT(shown.size() == 2 && shown[0] == a && shown[1] == b);

    g_results.clear();
    MessageDialogFinish(b, LAUFEY_MESSAGE_DIALOG_ANSWERED, 1);
    MessageDialogFinish(b, LAUFEY_MESSAGE_DIALOG_CANCELLED, -1);  // ignored
    EXPECT(g_results.size() == 1 && g_results[0].id == b &&
           g_results[0].status == LAUFEY_MESSAGE_DIALOG_ANSWERED &&
           g_results[0].response == 1);
    EXPECT(!MessageDialogIsOpen(b) && MessageDialogIsOpen(a));

    // An answer with no button is a failure, never button 0.
    MessageDialogFinish(a, LAUFEY_MESSAGE_DIALOG_ANSWERED, -1);
    EXPECT(g_results.size() == 2 &&
           g_results[1].status == LAUFEY_MESSAGE_DIALOG_FAILED &&
           g_results[1].response == -1);
    EXPECT(OpenMessageDialogCountForTesting() == 0);

    // A cancel carries no response.
    queued.clear();
    uint32_t c =
        ShowMessageDialogCommon(&ok, OnResult, &g_results, later, show);
    MessageDialogFinish(c, LAUFEY_MESSAGE_DIALOG_CANCELLED, 3);
    EXPECT(g_results.size() == 3 &&
           g_results[2].status == LAUFEY_MESSAGE_DIALOG_CANCELLED &&
           g_results[2].response == -1);
    // Unknown ids are ignored.
    MessageDialogFinish(0, LAUFEY_MESSAGE_DIALOG_ANSWERED, 0);
    MessageDialogFinish(999999, LAUFEY_MESSAGE_DIALOG_ANSWERED, 0);
    EXPECT(g_results.size() == 3);
  }

  std::printf("message_dialog_test: ok\n");
  return 0;
}
