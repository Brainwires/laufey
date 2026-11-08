// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Linux message dialogs (API 54): GtkMessageDialog, shown on the GTK thread
// without a nested loop (the "response" signal finishes it), so the backend
// keeps running while it is up. Both backends link GTK 3. With no parent the
// dialog is a plain app-modal top-level, which needs no window of the app's.

#include <gtk/gtk.h>

#include "laufey_backend_common.h"
#include "laufey_io.h"
#include "laufey_message_dialog.h"
#include "laufey_window.h"

#include <algorithm>
#include <string>
#include <vector>

namespace laufey_common {

namespace {

struct LinuxMessageDialog {
  uint32_t id = 0;
  size_t button_count = 0;
};

// The dialogs open now, so a quit can end them. GTK thread only.
std::vector<GtkWidget*>& OpenGtkMessageDialogs() {
  static auto* dialogs = new std::vector<GtkWidget*>;
  return *dialogs;
}

void OnResponse(GtkDialog* dialog, gint response, gpointer data) {
  auto* state = static_cast<LinuxMessageDialog*>(data);
  std::vector<GtkWidget*>& open = OpenGtkMessageDialogs();
  open.erase(std::remove(open.begin(), open.end(), GTK_WIDGET(dialog)),
             open.end());
  uint32_t id = state->id;
  size_t count = state->button_count;
  delete state;
  // A destroyed dialog emits no further responses.
  gtk_widget_destroy(GTK_WIDGET(dialog));
  if (response >= 0 && static_cast<size_t>(response) < count) {
    MessageDialogFinish(id, LAUFEY_MESSAGE_DIALOG_ANSWERED, response);
  } else {
    // GTK_RESPONSE_DELETE_EVENT (the close button, Escape with no cancel
    // button) or the quit below.
    MessageDialogFinish(id, LAUFEY_MESSAGE_DIALOG_CANCELLED, -1);
  }
}

gboolean CancelOpenMessageDialogs(gpointer) {
  for (GtkWidget* dialog : std::vector<GtkWidget*>(OpenGtkMessageDialogs()))
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_DELETE_EVENT);
  return G_SOURCE_REMOVE;
}

GtkMessageType MessageType(int kind) {
  switch (kind) {
    case LAUFEY_MESSAGE_DIALOG_WARNING:
      return GTK_MESSAGE_WARNING;
    case LAUFEY_MESSAGE_DIALOG_ERROR:
      return GTK_MESSAGE_ERROR;
    default:
      return GTK_MESSAGE_INFO;
  }
}

void ShowGtk(uint32_t id, GtkWindow* parent, const MessageDialogRequest& req) {
  if (IsQuitting() || !gdk_display_get_default()) {
    MessageDialogFinish(id, LAUFEY_MESSAGE_DIALOG_FAILED, -1);
    return;
  }
  GtkWidget* dialog = gtk_message_dialog_new(
      parent,
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      MessageType(req.kind), GTK_BUTTONS_NONE, "%s", req.message.c_str());
  if (!req.detail.empty()) {
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
                                             req.detail.c_str());
  }
  if (!req.title.empty())
    gtk_window_set_title(GTK_WINDOW(dialog), req.title.c_str());
  for (size_t i = 0; i < req.buttons.size(); i++) {
    gtk_dialog_add_button(GTK_DIALOG(dialog),
                          GtkButtonLabel(req.buttons[i]).c_str(),
                          static_cast<gint>(i));
  }
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), req.default_index);
  if (!parent) {
    // No window to sit over: show it where the user is looking.
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
    gtk_window_set_keep_above(GTK_WINDOW(dialog), TRUE);
  }
  auto* state = new LinuxMessageDialog{id, req.buttons.size()};
  // Escape presses the cancel button: GTK's dialog sends DELETE_EVENT for
  // Escape, which "close" turns into a response; with a cancel button that
  // response is the cancel button's instead.
  if (req.cancel_index >= 0) {
    g_object_set_data(G_OBJECT(dialog), "laufey-cancel-index",
                      GINT_TO_POINTER(req.cancel_index + 1));
    g_signal_connect(dialog, "close", G_CALLBACK(+[](GtkDialog* d, gpointer) {
                       gint cancel = GPOINTER_TO_INT(g_object_get_data(
                                         G_OBJECT(d), "laufey-cancel-index")) -
                                     1;
                       g_signal_stop_emission_by_name(d, "close");
                       gtk_dialog_response(d, cancel);
                     }),
                     nullptr);
  }
  g_signal_connect(dialog, "response", G_CALLBACK(OnResponse), state);
  OpenGtkMessageDialogs().push_back(dialog);
  gtk_widget_show_all(dialog);
  gtk_window_present(GTK_WINDOW(dialog));
}

}  // namespace

void CancelGtkMessageDialogsForQuit() {
  g_idle_add_full(G_PRIORITY_HIGH, CancelOpenMessageDialogs, nullptr, nullptr);
}

uint32_t ShowMessageDialogLinux(ParentResolver parent,
                                const laufey_message_dialog_options_t* options,
                                laufey_message_dialog_result_fn callback,
                                void* user_data) {
  return ShowMessageDialogCommon(
      options, callback, user_data,
      [](std::function<void()> fn) { GtkRunAsync(std::move(fn)); },
      [parent](uint32_t id, const MessageDialogRequest& request) {
        GtkWindow* window =
            parent ? static_cast<GtkWindow*>(parent()) : nullptr;
        ShowGtk(id, window, request);
      });
}

}  // namespace laufey_common
