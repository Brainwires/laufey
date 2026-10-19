// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The CLIPBOARD selection of an X11 display, spoken directly over a private
// XCB connection. Two sessions use it:
//   - X11 sessions (no Wayland display): the clipboard of the X server the
//     app's windows are on, read and set off the UI thread, whatever has
//     focus, as on macOS and Windows (GTK's clipboard would block the UI
//     thread on every read, and can't be reached from CEF's threads);
//   - GNOME's mutter, which has no ext-data-control: its Xwayland selection
//     bridge copies the clipboard between X11 and Wayland in both directions
//     whatever has focus, so laufey is an X11 client of Xwayland.
// Nothing in GTK or the process environment changes; the connection is
// opened on the first clipboard call (which may start Xwayland, as mutter
// does on demand) and served by a thread of its own, never the UI thread.
//
// ICCCM: reads convert TARGETS, then the first wanted target, following
// INCR for large transfers, each step with a deadline. As the owner, laufey
// answers TARGETS, TIMESTAMP and every entry's type (MULTIPLE is refused),
// in one property or through INCR for large data (at most 8 transfers at
// once, each with deadlines), off the UI thread. Once it owns the selection
// it asks a running clipboard manager (the owner of CLIPBOARD_MANAGER) to
// save it (SAVE_TARGETS), so the data outlives the app, as GTK's
// gtk_clipboard_store does.

#ifndef LAUFEY_CLIPBOARD_X11_LINUX_H_
#define LAUFEY_CLIPBOARD_X11_LINUX_H_

#include <functional>
#include <string>
#include <vector>

#include "clipboard_data_control_linux.h"

namespace laufey_common {
namespace x11_clipboard {

// Whether the clipboard goes through X11 here (UseX11Bridge, with this
// process's display and environment). Connects on the first call, on the
// bridge's own thread (a few seconds at most; Xwayland may be starting).
bool Available();

// The decision (pure; tests): never with LAUFEY_CLIPBOARD=gtk or without an
// X11 display ($DISPLAY) to connect to; an X11 session (`display_backend`
// "x11", DisplayBackend's answer); a Wayland one whose compositor is mutter
// (asked only then).
bool UseX11Bridge(const std::string& display_backend, const char* display,
                  const char* laufey_clipboard,
                  const std::function<bool()>& compositor_is_mutter);

// Tests: SAVE_TARGETS requests sent to a clipboard manager, and the ones it
// answered as saved.
int SaveRequestsForTesting();
int SavesConfirmedForTesting();

// The same contract as data_control::Types / Read / Write / Watch. Read's
// `mimes` are target names; *mime_out is the target read, and *type_out
// (optional) the property type the owner answered with (STRING is Latin-1).
bool Types(std::vector<std::string>* out);
bool Read(const std::vector<std::string>& mimes, size_t max_bytes,
          std::string* out, std::string* mime_out, bool* found,
          std::string* type_out = nullptr);
bool Write(data_control::Entries entries);
void Watch(bool on);

// Tests: use the bridge on whatever X11 display DISPLAY names, whatever the
// compositor (call before the first use).
void EnableForTesting();
// Tests: outgoing INCR transfers in flight.
int ActiveTransfers();
// Tests: how long after the connection failed the one reconnect may happen
// (30 s by default).
void SetReconnectBackoffForTesting(int ms);

}  // namespace x11_clipboard
}  // namespace laufey_common

#endif  // LAUFEY_CLIPBOARD_X11_LINUX_H_
