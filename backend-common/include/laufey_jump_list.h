// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// The dock menu on Windows: set_dock_menu as the "Tasks" category of the
// app's taskbar jump list (ICustomDestinationList), shared by the CEF and
// WebView2 backends.
//
// A jump-list task is a shell link the shell runs, so a click starts the
// executable again, with one argument: kDockItemArgPrefix followed by the
// item's id ("--laufey-dock-item=new-window"). With the single-instance lock
// on, that launch is forwarded to the running app, and the interceptor this
// file installs (SetSecondInstanceInterceptor) turns it into the dock menu's
// click (on_click with window id 0, as on macOS) instead of a second-instance
// launch. Without the lock, or before the app set its dock menu in this run
// (the jump list outlives the process), the argument reaches the embedder as
// a launch argument (its own command line, or the second-instance handler):
// DockItemFromArgs recognises it.
//
// A jump list is flat and can't grey an item out, so the menu is flattened
// (JumpListTasksFromMenu): a submenu's items are listed in its place between
// separators, disabled items and roles are left out, and "checked" is not
// shown. See docs/dock-taskbar.md.

#ifndef LAUFEY_JUMP_LIST_H_
#define LAUFEY_JUMP_LIST_H_

#include <string>
#include <vector>

#include "laufey.h"
#include "laufey_menu.h"
#include "laufey_single_instance.h"

namespace laufey_common {

// The argument a jump-list task starts the app with, before the item's id.
inline constexpr char kDockItemArgPrefix[] = "--laufey-dock-item=";

// The longest id a task carries (bytes); an item with a longer one is left
// out of the jump list. A shell link's arguments hold at most 1024
// characters (INFOTIPSIZE), the flag and the quotes included.
inline constexpr size_t kMaxDockItemIdBytes = 512;

// One entry of the jump list's Tasks category.
struct JumpListTask {
  bool separator = false;
  std::string id;     // what a click reports (MenuEntry::ClickKey)
  std::string label;  // the task's title: the label, else the id
};

// The Tasks category for a parsed dock menu: enabled items with a click key
// (the id, else the label) of at most kMaxDockItemIdBytes, in order, each
// submenu's items in its place between separators, roles and disabled items
// left out. Separators never lead, trail or repeat.
std::vector<JumpListTask> JumpListTasksFromMenu(
    const std::vector<MenuEntry>& entries);

// Whether `args` (argv after the program) are a jump-list task's launch:
// exactly one argument, kDockItemArgPrefix followed by a non-empty id of at
// most kMaxDockItemIdBytes. Sets `*id` then. Anything else (more arguments,
// the flag after "--", an empty id) is not one, so a deep link can't pass for
// a task.
bool DockItemFromArgs(const std::vector<std::string>& args, std::string* id);

// `arg` quoted for the Windows command line, so CommandLineToArgvW (and the
// CRT) read it back as one argument, unchanged.
std::string QuoteWindowsArgument(const std::string& arg);

// Where the dock menu's clicks go: the ids now on the menu and the backend's
// click callback. Installs the second-instance interceptor; on_click NULL
// removes it (a task's launch then reaches the second-instance handler).
void SetDockItemHandler(const std::vector<JumpListTask>& tasks,
                        laufey_menu_click_fn on_click, void* on_click_data);

// The interceptor: a jump-list task's launch (DockItemFromArgs) while a
// handler is set calls it with the id (window id 0) when the id is on the
// menu, and is taken (true) either way, an id that is no longer on the menu
// being dropped. Anything else returns false. Exposed for tests.
bool DeliverDockItemLaunch(const SecondInstanceMessage& message);

#ifdef _WIN32
// Backs set_dock_menu on Windows: parses `menu_template` (the caller's only
// for this call), replaces the jump list's Tasks with it (NULL, or a menu with
// no task, removes them) and routes the tasks' clicks to `on_click`. The
// jump list itself is written on a worker thread of its own (COM, STA). It is
// the taskbar button's for the process's AppUserModelID: the explicit one
// when the process set one, else the one Windows derives from the executable.
void SetDockMenuWin(laufey_value_t* menu_template,
                    const laufey_backend_api_t* api,
                    laufey_menu_click_fn on_click, void* on_click_data);
#endif

}  // namespace laufey_common

#endif  // LAUFEY_JUMP_LIST_H_
